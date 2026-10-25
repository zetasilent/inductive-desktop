// Bounded evidence records and deterministic checks. The model never supplies a live netlist.
#ifndef INDUCTIVE_REVIEW_H
#define INDUCTIVE_REVIEW_H
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <regex>
namespace INDUCTIVE_REVIEW
{
using json = nlohmann::json;
inline std::string Hash(const std::string& text) { return picosha2::hash256_hex_string(text); }
inline std::string Text(const json& j, const char* key, size_t max, bool required = true)
{
    if(!j.contains(key) && !required) return "";
    if(!j.contains(key) || !j[key].is_string()) throw std::runtime_error(std::string(key) + " must be text.");
    const auto s = j[key].get<std::string>();
    if((required && s.find_first_not_of(" \t\r\n") == std::string::npos) || s.size() > max || s.find('\0') != std::string::npos)
        throw std::runtime_error(std::string(key) + " is missing or too long.");
    return s;
}
inline double Number(const json& j, const char* key)
{
    if(!j.contains(key) || !j[key].is_number()) throw std::runtime_error(std::string(key) + " must be a number.");
    const double v=j[key].get<double>();
    if(!std::isfinite(v) || std::abs(v)>1000000) throw std::runtime_error("Specification must be finite and within ±1,000,000 base units.");
    return v;
}
inline json Facts(const json& input, size_t pages, bool human)
{
    if(!input.is_array() || input.size()>128) throw std::runtime_error("Use at most 128 specification records.");
    json out=json::array(); std::set<std::string> ids;
    for(const auto& f:input)
    {
        if(!f.is_object()) throw std::runtime_error("Each specification must be an object.");
        json r;
        r["id"]=Text(f,"id",80); if(!ids.insert(r["id"]).second) throw std::runtime_error("Duplicate specification id.");
        const auto kind=Text(f,"kind",20);
        const bool voltage=kind=="supply" || kind=="input" || kind=="output" || kind=="voltage_rating";
        if(!voltage && kind!="connection" && kind!="support" && kind!="power_rating") throw std::runtime_error("Unknown electrical specification kind.");
        r["kind"]=kind; r["pin"]=Text(f,"pin",40); r["ground_pin"]=Text(f,"ground_pin",40);
        if(r["pin"]==r["ground_pin"]) throw std::runtime_error("Signal/supply and reference pins must be different.");
        r["device"]=Text(f,"device",200); r["variant"]=Text(f,"variant",200);
        r["conditions"]=Text(f,"conditions",1000); r["quote"]=Text(f,"quote",1000);
        if(!f.contains("page") || !f["page"].is_number_integer() || f["page"]<1 || f["page"]>pages)
            throw std::runtime_error("Cite a physical page within the source PDF.");
        r["page"]=f["page"];
        const std::string unit=voltage?"V":kind=="power_rating"?"W":kind=="connection"?"connection":Text(f,"support_type",20)=="resistor"?"ohm":"F";
        if(f.contains("unit") && (!f["unit"].is_string() || f["unit"]!=unit)) throw std::runtime_error("Use the specification's base unit: "+unit);
        r["unit"]=unit;
        if(voltage) {
            r["min_v"]=Number(f,"min_v"); r["max_v"]=Number(f,"max_v");
            if(r["min_v"].get<double>()>r["max_v"].get<double>()) throw std::runtime_error("Minimum voltage exceeds maximum voltage.");
            if(kind=="voltage_rating" && (r["min_v"].get<double>()!=0 || r["max_v"].get<double>()<=0)) throw std::runtime_error("Passive voltage rating requires min_v=0 and a positive max_v; polarity is not checked.");
        }
        if(kind=="connection") {
            auto relation=Text(f,"relation",20);
            if(relation!="connected" && relation!="same_net" && relation!="different_net") throw std::runtime_error("Choose connected, same_net or different_net.");
            r["relation"]=relation;
        }
        if(kind=="support") {
            auto type=Text(f,"support_type",20);
            if(type!="resistor" && type!="capacitor") throw std::runtime_error("Choose resistor or capacitor support.");
            r["support_type"]=type;r["min_value"]=Number(f,"min_value");r["max_value"]=Number(f,"max_value");
            if(r["min_value"].get<double>()<=0 || r["min_value"].get<double>()>r["max_value"].get<double>()) throw std::runtime_error("Support value bounds must be positive and ordered.");
        }
        if(kind=="power_rating") {
            r["rated_w"]=Number(f,"rated_w");r["tolerance_pct"]=Number(f,"tolerance_pct");
            if(r["rated_w"].get<double>()<=0 || r["tolerance_pct"].get<double>()<0 || r["tolerance_pct"].get<double>()>=100) throw std::runtime_error("Use a positive resistor power rating and tolerance from 0 to less than 100 percent.");
        }
        // Logic fields are optional as a pair. Typical values and absolute maxima are not accepted here.
        const char* low=kind=="input" ? "vil_max_v" : "vol_max_v";
        const char* high=kind=="input" ? "vih_min_v" : "voh_min_v";
        if((kind=="input" || kind=="output") && (f.contains(low) || f.contains(high)))
        {
            const auto l=Number(f,low), h=Number(f,high);
            if(l>h || l<r["min_v"].get<double>() || h>r["max_v"].get<double>()) throw std::runtime_error("Logic limits must be ordered and inside the operating voltage envelope.");
            r[low]=l; r[high]=h;
        }
        if(kind=="output")
        {
            auto drive=Text(f,"drive",30);
            if(drive!="push_pull" && drive!="open_drain" && drive!="other") throw std::runtime_error("Choose push_pull, open_drain or other drive.");
            r["drive"]=drive;
        }
        r["approved"]=human && f.value("approved",false);
        out.push_back(r);
    }
    return out;
}

// Only explicit native library types qualify as passive topology. A reference prefix is not evidence.
inline bool Passive(const json& c,const std::string& type)
{
    const auto lib=c.value("lib_id","");
    return !c.value("excluded",false) && !c.value("ambiguous",false) && c.at("pins").size()==2 &&
        (type=="resistor" ? (lib=="Device:R" || lib=="Device:R_Small") :
         (lib=="Device:C" || lib=="Device:C_Small" || lib=="Device:C_Polarized" || lib=="Device:C_Polarized_Small"));
}
inline double PassiveValue(const std::string& value,const std::string& type)
{
    // Deliberately reject ranges, tolerances, annotations and ambiguous unit suffixes.
    std::smatch m;std::string s=value;
    s=std::regex_replace(s,std::regex("^\\s+|\\s+$"),"");
    for(const std::string micro:{std::string("µ"),std::string("μ")})for(size_t at=0;(at=s.find(micro,at))!=std::string::npos;)s.replace(at,micro.size(),"u");
    double multiplier=1,number=0;
    static const std::regex ordinary(R"(^([0-9]+(?:\.[0-9]+)?|\.[0-9]+)\s*(Meg|[RrkKMmunp]?)(ohm|Ohm|Ω|F)?$)");
    static const std::regex embedded(R"(^([0-9]+)([RrkKMmunp])([0-9]+)$)");
    std::string prefix,suffix;
    try {
        if(std::regex_match(s,m,ordinary)) {number=std::stod(m[1]);prefix=m[2];suffix=m[3];}
        else if(std::regex_match(s,m,embedded)) {number=std::stod(m[1].str()+"."+m[3].str());prefix=m[2];}
        else return 0;
    } catch(const std::exception&) {return 0;}
    if(type=="resistor" && (suffix=="F" || prefix=="n" || prefix=="p")) return 0;
    if(type=="capacitor" && prefix.empty() && suffix.empty())return 0;
    if(type=="capacitor" && ((!suffix.empty() && suffix!="F") || prefix=="R" || prefix=="r" || prefix=="k" || prefix=="K" || prefix=="M" || prefix=="Meg")) return 0;
    if(prefix=="k" || prefix=="K")multiplier=1e3;else if(prefix=="M" || prefix=="Meg")multiplier=1e6;
    else if(prefix=="m")multiplier=1e-3;else if(prefix=="u")multiplier=1e-6;else if(prefix=="n")multiplier=1e-9;else if(prefix=="p")multiplier=1e-12;
    const auto result=number*multiplier;return std::isfinite(result) && result>0?result:0;
}
inline bool VoltageAcross(const json& rails,const std::string& a,const std::string& b,double& worst)
{
    if(a.empty() || b.empty())return false;
    if(a==b){worst=0;return true;}
    for(const auto& r:rails) if((r["net"]==a && r["reference_net"]==b) || (r["net"]==b && r["reference_net"]==a))
    {worst=std::max(std::abs(r.at("min_v").get<double>()),std::abs(r.at("max_v").get<double>()));return true;}
    for(const auto& x:rails)for(const auto& y:rails)if(x["net"]==a && y["net"]==b && x["reference_net"]==y["reference_net"])
    {worst=std::max(std::abs(x.at("min_v").get<double>()-y.at("max_v").get<double>()),std::abs(x.at("max_v").get<double>()-y.at("min_v").get<double>()));return true;}
    return false;
}
inline json Rails(const json& input)
{
    if(!input.is_array() || input.size()>128) throw std::runtime_error("Use at most 128 declared net voltage ranges.");
    json out=json::array(); std::set<std::string> nets;
    for(const auto& r:input)
    {
        auto net=Text(r,"net",500), ground=Text(r,"reference_net",500);
        if(net==ground || !nets.insert(net).second) throw std::runtime_error("Declare one range per net, relative to a different reference net.");
        const auto low=Number(r,"min_v"), high=Number(r,"max_v");
        if(low>high) throw std::runtime_error("Net minimum exceeds maximum.");
        out.push_back({{"net",net},{"reference_net",ground},{"min_v",low},{"max_v",high},{"conditions",Text(r,"conditions",1000)}});
    }
    return out;
}
inline std::string SourceDigest(const json& source)
{
    return Hash(json{{"sha256",source.value("pdf_sha256","")},{"specifications",source.value("specifications",json::array())}}.dump());
}
inline json Binding(const json& component,const json& source,const std::string& context)
{
    if(source.value("kind","")!="datasheet" || source.value("archived",false) || source.value("pdf_sha256","").empty())
        throw std::runtime_error("Save specification records on an active PDF before binding it.");
    if(component.value("ambiguous",false)) throw std::runtime_error("Resolve duplicate or unannotated references before binding.");
    return {{"component_id",component.at("id")},{"identity",component.at("identity")},{"source_id",source.at("id")},
            {"source_digest",SourceDigest(source)},{"context",Text(json{{"context",context}},"context",1000)}};
}
inline std::string MatchKey(std::string s)
{
    const auto first=s.find_first_not_of(" \t\r\n"),last=s.find_last_not_of(" \t\r\n");
    if(first==std::string::npos)return "";s=s.substr(first,last-first+1);
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::toupper(c);});return s;
}
inline json Setup(const json& snapshot,const json& state,const json& inventory)
{
    json sheets=json::object();for(const auto& p:inventory)if(p.value("kind","")=="datasheet" && !p.value("archived",false))sheets[p.at("id").get<std::string>()]=p;
    json result={{"fingerprint",Hash(snapshot.dump()+state.dump()+inventory.dump())},{"components",json::array()},{"missing_rails",json::array()},{"datasheets",json::object()}};
    for(const auto& [sid,source]:sheets.items())result["datasheets"][sid]={{"source_digest",SourceDigest(source)}};
    std::set<std::string> railPairs;
    for(const auto& c:snapshot.at("components"))if(!c.value("excluded",false)) {
        const auto cid=c.at("id").get<std::string>();const auto value=MatchKey(c.value("value","")),mpn=MatchKey(c.value("mpn",""));
        json candidates=json::object();
        auto candidate=[&](const std::string& sid,const std::string& reason) {
            if(!sheets.contains(sid))return;
            const auto& source=sheets.at(sid);size_t approved=0;json devices=json::array();
            for(const auto& f:source.value("specifications",json::array()))if(f.value("approved",false)) {++approved;devices.push_back({{"device",f.value("device","")},{"variant",f.value("variant","")},{"conditions",f.value("conditions","")}});}
            candidates[sid]={{"source_id",sid},{"name",source.value("name","")},{"source_digest",SourceDigest(source)},{"approved_count",approved},{"reason",reason},{"applicability",devices}};
        };
        const auto bindings=state.value("bindings",json::object());bool current=false;
        if(bindings.contains(cid)) {
            const auto& b=bindings.at(cid);const auto sid=b.value("source_id","");candidate(sid,"Previously linked to this component");
            current=sheets.contains(sid) && candidates.at(sid).at("approved_count").get<size_t>()>0 && b.value("identity","")==c.value("identity","") && b.value("source_digest","")==SourceDigest(sheets.at(sid));
        }
        for(const auto& p:inventory)if(!p.value("archived",false)) {
            const auto key=MatchKey(p.value("mpn",""));
            if(!key.empty() && (key==(mpn.empty()?value:mpn)))candidate(p.value("kind","")=="datasheet"?p.value("id",""):p.value("datasheet_id",""),"Exact saved manufacturer part number");
        }
        for(const auto& [sid,source]:sheets.items())for(const auto& f:source.value("specifications",json::array())) {
            const auto key=MatchKey(f.value("device",""));
            if(!key.empty() && (key==(mpn.empty()?value:mpn))) {candidate(sid,"Exact device name in saved specifications");break;}
        }
        json options=json::array();for(const auto& [sid,p]:candidates.items())options.push_back(p);
        const auto status=c.value("ambiguous",false)?"identity_ambiguous":current?"confirmed":options.empty()?"missing_evidence":options.size()>1?"ambiguous":"suggested";
        result["components"].push_back({{"component_id",cid},{"reference",c.at("reference")},{"value",c.value("value","")},{"mpn",c.value("mpn","")},{"footprint",c.value("footprint","")},{"status",status},{"candidates",options}});
        // Supply limits identify requirements that need input. They never establish the board's intended voltage.
        const std::string selected=current?bindings.at(cid).value("source_id",""):options.size()==1?options[0].value("source_id",""):"";
        if(selected.empty())continue;
        for(const auto& f:sheets.at(selected).value("specifications",json::array()))if(f.value("approved",false) && f.value("kind","")=="supply") {
            const json *pin=nullptr,*ground=nullptr;
            for(const auto& p:c.at("pins")) {if(p.value("number","")==f.value("pin",""))pin=&p;if(p.value("number","")==f.value("ground_pin",""))ground=&p;}
            if(!pin || !ground)continue;const auto a=pin->value("net",""),b=ground->value("net","");
            if(a.empty() || b.empty() || a==b)continue;
            bool exists=false;for(const auto& r:state.value("rails",json::array()))exists|=r["net"]==a && r["reference_net"]==b;
            if(!exists && railPairs.insert(json::array({a,b}).dump()).second)result["missing_rails"].push_back({{"net",a},{"reference_net",b},{"component",c.at("reference")},{"reason","Approved supply pin needs an engineer-declared operating voltage, including tolerance and conditions."}});
        }
    }
    return result;
}
inline std::string Fingerprint(const json& snapshot,const json& state,const json& sources)
{
    return Hash(json{{"engine","electrical-review-2"},{"design",snapshot.at("fingerprint")},
                     {"project",snapshot.value("project","")},
                     {"rails",state.value("rails",json::array())},{"bindings",state.value("bindings",json::object())},{"sources",sources}}.dump());
}
inline const json* Pin(const json& component,const std::string& number)
{
    for(const auto& p:component.at("pins")) if(p.value("number","")==number) return &p;
    return nullptr;
}
inline json Run(const json& snapshot,const json& state,const json& sources)
{
    const auto rails=Rails(state.value("rails",json::array()));
    json report={{"version",2},{"engine","electrical-review-2"},{"fingerprint",Fingerprint(snapshot,state,sources)},
                 {"design_fingerprint",snapshot.at("fingerprint")},{"findings",json::array()},
                 {"scope","Supply ranges; direct push-pull voltage/logic; open-drain pull-up topology; required pin connections; direct resistor/capacitor support values; passive voltage ratings and resistor DC power; numbered footprint pads. Supported matches cover only the named check under confirmed conditions. No simulation, transient, thermal, timing, signal-integrity, layout or complete hardware validation."},
                 {"snapshot",snapshot},{"requirements",rails},{"bindings",state.value("bindings",json::object())},{"sources",sources}};
    auto& findings=report["findings"];
    auto add=[&](std::string rule,std::string status,const json& c,std::string title,std::string detail,json evidence=json::array(),std::string net="",std::string discriminator="") {
        json f={{"id",Hash(rule+c.at("id").get<std::string>()+net+discriminator)},{"rule",rule},{"status",status},{"component_id",c.at("id")},{"reference",c.at("reference")},
                {"title",title},{"detail",detail},{"net",net},{"evidence",evidence}};
        findings.push_back(f);
    };
    auto evidence=[](const json& s,const json& f) {auto e=f; e["source_id"]=s.at("id"); e["filename"]=s.at("name"); e["pdf_sha256"]=s.value("pdf_sha256",""); return e;};
    struct REVIEW_TERMINAL {const json* component; json fact,source; std::string ground;};
    std::map<std::string,std::vector<REVIEW_TERMINAL>> inputs,outputs;
    std::map<std::string,std::set<std::string>> terminals;
    const auto bindings=state.value("bindings",json::object());
    for(const auto& c:snapshot.at("components"))
        for(const auto& p:c.at("pins")) if(!p.value("net","").empty()) terminals[p["net"]].insert(c.at("id").get<std::string>()+":"+p.at("number").get<std::string>());
    for(const auto& c:snapshot.at("components"))
    {
        if(c.value("excluded",false)) continue;
        if(c.value("ambiguous",false)) {add("identity","needs_investigation",c,"Resolve component identity","Duplicate references, repeated units or unannotated symbols cannot be reviewed safely.");continue;}
        if(!c.value("footprint_error","").empty()) add("pin_mapping","not_checked",c,"Footprint could not be checked",c["footprint_error"]);
        else
        {
            std::set<std::string> pads; for(const auto& p:c.at("pads")) pads.insert(p);
            std::string missing;
            for(const auto& p:c.at("pins")) if(!pads.count(p.at("number"))) missing+=(missing.empty()?"":", ")+p.at("number").get<std::string>();
            add("pin_mapping",missing.empty()?"supported_match":"supported_violation",c,missing.empty()?"Numbered pins have footprint pads":"Footprint is missing numbered pins",
                missing.empty()?"All schematic pin numbers occur in the assigned footprint file. Package dimensions, pad functions and physical orientation are not checked.":"Missing pads: "+missing+". Assign a compatible package.",
                json::array({{{"footprint",c.value("footprint","")},{"sha256",c.value("footprint_sha256","")},{"symbol_pins",c.at("pins")},{"footprint_pads",c.at("pads")}}}));
        }
        const auto cid=c.at("id").get<std::string>();
        if(!bindings.contains(cid)) {add("evidence","not_checked",c,"Electrical evidence not linked","Bind an approved datasheet and confirm the device, variant and operating conditions.");continue;}
        const auto& b=bindings.at(cid); const auto sid=b.value("source_id","");
        if(!sources.contains(sid) || sources[sid].contains("error") || sources[sid].value("archived",false)) {add("evidence","not_checked",c,"Source unavailable","The linked PDF is removed, damaged or unavailable.");continue;}
        const auto& source=sources.at(sid);
        if(b.value("identity","")!=c.at("identity").get<std::string>() || b.value("source_digest","")!=SourceDigest(source)) {add("evidence","needs_investigation",c,"Binding needs confirmation","The component identity or its source facts changed. Review and confirm the binding again.");continue;}
        const auto facts=source.value("specifications",json::array());
        std::set<std::string> targets; bool conflict=false;
        for(const auto& f:facts) if(f.value("approved",false) && !targets.insert(f.value("kind","")+":"+f.value("pin","")).second) conflict=true;
        if(conflict) {add("evidence","needs_investigation",c,"Conflicting approved specifications","Multiple approved records apply to the same pin and check. Resolve variants or conditions before checking.");continue;}
        bool any=false, supply=false, interfacing=false;
        for(const auto& f:facts)
        {
            if(!f.value("approved",false)) continue;
            any=true; const auto kind=f.value("kind",""); supply|=kind=="supply"; interfacing|=kind=="input" || kind=="output";
            const auto* pin=Pin(c,f.value("pin","")); const auto* ground=Pin(c,f.value("ground_pin",""));
            auto e=json::array({evidence(source,f)});
            if(kind=="connection")
            {
                if(!pin || !ground) {add(kind,"needs_investigation",c,"Required numbered pin is missing","The approved record does not match the instantiated package pins.",e,"",f.value("id",""));continue;}
                const auto a=pin->value("net",""),z=ground->value("net","");
                const auto relation=f.at("relation").get<std::string>();
                const bool wired=!a.empty() && terminals[a].size()>1;
                const bool ok=relation=="connected"?wired:relation=="same_net"?wired && a==z:wired && !z.empty() && a!=z;
                add(kind,ok?"supported_match":"supported_violation",c,ok?"Required pin connection is present":"Required pin connection is missing or incorrect",
                    "Pin "+f.at("pin").get<std::string>()+" requires "+relation+" relative to pin "+f.at("ground_pin").get<std::string>()+". Checks native connectivity only; component suitability and signal behavior need separate review.",e,a,f.value("id",""));continue;
            }
            if(kind=="support" || kind=="voltage_rating" || kind=="power_rating")
            {
                if(!pin || !ground || pin->value("net","").empty() || ground->value("net","").empty())
                {add(kind,"needs_investigation",c,"Specification terminals are unavailable","Both numbered terminals must have native connectivity.",e,"",f.value("id",""));continue;}
                const auto a=pin->at("net").get<std::string>(),z=ground->at("net").get<std::string>();
                if(kind=="support") {
                    bool present=false,unknown=false,ok=false;const auto type=f.at("support_type").get<std::string>();
                    if(a==z) {add(kind,"needs_investigation",c,"Support terminals are shorted","Cannot check a support component across the same net.",e,a,f.value("id",""));continue;}
                    for(const auto& p:snapshot.at("components")) if(Passive(p,type)) {
                        const auto x=p["pins"][0].value("net",""),y=p["pins"][1].value("net","");
                        if(!((x==a && y==z) || (x==z && y==a)))continue;
                        present=true;const auto v=PassiveValue(p.value("value",""),type);unknown|=v==0;
                        ok|=v>0 && v>=f.at("min_value").get<double>() && v<=f.at("max_value").get<double>();
                        e.push_back({{"reference",p.at("reference")},{"value",p.value("value","")},{"nominal_base_units",v},{"pins",p.at("pins")}});
                    }
                    for(const auto& p:snapshot.at("components")) if(p.at("id")!=c.at("id") && !Passive(p,type) && p.at("pins").size()==2) {
                        const auto x=p["pins"][0].value("net",""),y=p["pins"][1].value("net","");
                        if((x==a && y==z) || (x==z && y==a))unknown=true;
                    }
                    add(kind,ok?"supported_match":unknown?"needs_investigation":"supported_violation",c,ok?"Required support component nominal value is present":present?"Support component value needs correction or review":"Required direct support component is missing",
                        "Requires one directly connected "+type+" between pins "+f.at("pin").get<std::string>()+" and "+f.at("ground_pin").get<std::string>()+" with nominal value "+std::to_string(f.at("min_value").get<double>())+"–"+std::to_string(f.at("max_value").get<double>())+" "+f.at("unit").get<std::string>()+". Checks standard Device symbols and nominal values; tolerance, ESR, effective capacitance, stability and placement are not checked.",e,a,f.value("id",""));
                } else {
                    double v=0;
                    if(!VoltageAcross(rails,a,z,v)) {add(kind,"not_checked",c,"Voltage across the component is undeclared","Declare terminal voltage ranges using a common reference; net labels do not establish voltage.",e,a,f.value("id",""));continue;}
                    e.push_back({{"requirements",rails},{"worst_case_voltage_v",v}});
                    double demand=v,limit=f.value("max_v",0.0);
                    if(kind=="power_rating") {
                        const auto resistance=Passive(c,"resistor")?PassiveValue(c.value("value",""),"resistor"):0;
                        if(!resistance) {add(kind,"not_checked",c,"Resistor value or symbol is unsupported","DC power requires a standard two-terminal resistor and an unambiguous resistance value.",e,a,f.value("id",""));continue;}
                        demand=v*v/(resistance*(1-f.at("tolerance_pct").get<double>()/100));limit=f.at("rated_w");
                        if(!std::isfinite(demand)) {add(kind,"needs_investigation",c,"DC power calculation exceeds numeric limits","Review the resistance value and operating requirements; no finite power result is available.",e,a,f.value("id",""));continue;}
                        e.push_back({{"nominal_resistance_ohm",resistance},{"worst_case_dc_power_w",demand},{"allowed_power_w",limit}});
                    } else if(!Passive(c,"resistor") && !Passive(c,"capacitor")) {
                        add(kind,"not_checked",c,"Passive rating symbol is unsupported","Voltage rating checks support standard two-terminal resistor and capacitor symbols only.",e,a,f.value("id",""));continue;
                    }
                    const bool ok=demand<=limit;
                    add(kind,ok?"supported_match":"supported_violation",c,kind=="power_rating"?(ok?"Resistor DC power fits the approved rating":"Resistor DC power exceeds the approved rating"):(ok?"Terminal voltage fits the passive rating":"Terminal voltage exceeds the passive rating"),
                        "Conservative declared terminal voltage: "+std::to_string(v)+" V; computed demand "+std::to_string(demand)+(kind=="power_rating"?" W":" V")+"; approved limit "+std::to_string(limit)+". The engineer must approve the rating for actual temperature and derating conditions. Transients and capacitor polarity are not checked.",e,a,f.value("id",""));
                }
                continue;
            }
            if(!pin || !ground || pin->value("net","").empty() || ground->value("net","").empty() || (*pin)["net"]==(*ground)["net"])
            {add(kind,"needs_investigation",c,"Specification pins are missing or unsuitable","Both numbered pins must exist on distinct connected nets.",e,"",f.value("id",""));continue;}
            const auto net=pin->at("net").get<std::string>(), gnd=ground->at("net").get<std::string>();
            if(kind=="supply")
            {
                const json* rail=nullptr; for(const auto& r:rails) if(r["net"]==net && r["reference_net"]==gnd) rail=&r;
                if(!rail) {add("supply","not_checked",c,"Supply voltage is undeclared","Declare the operating range of "+net+" relative to "+gnd+". Net names are not voltage evidence.",e,net,f.value("id",""));continue;}
                const bool ok=rail->at("min_v").get<double>()>=f.at("min_v").get<double>() && rail->at("max_v").get<double>()<=f.at("max_v").get<double>();
                auto ev=e; ev.push_back({{"requirement",*rail},{"binding_context",b.value("context","")}});
                add("supply",ok?"supported_match":"supported_violation",c,ok?"Supply fits the approved operating range":"Supply exceeds the approved operating range",
                    "Declared "+std::to_string(rail->at("min_v").get<double>())+"–"+std::to_string(rail->at("max_v").get<double>())+" V; approved "+std::to_string(f.at("min_v").get<double>())+"–"+std::to_string(f.at("max_v").get<double>())+" V. Applies only under the confirmed conditions.",ev,net,f.value("id",""));
            }
            else if(kind=="input" || kind=="output") (kind=="input"?inputs:outputs)[net].push_back({&c,f,source,gnd});
        }
        if(!any) add("evidence","not_checked",c,"No approved specifications","Extract or enter facts and explicitly approve them against the source PDF.");
        if(!supply) add("supply","not_checked",c,"Supply specifications are not covered","No approved supply-pin record applies to this component.");
        if(!interfacing) add("interface","not_checked",c,"Interface specifications are not covered","No approved interface-pin record applies to this component.");
    }
    for(const auto& [net,drivers]:outputs) for(const auto& out:drivers) if(out.fact.value("drive","")=="open_drain")
    {
        auto ev=json::array({evidence(out.source,out.fact)});bool pullup=false,unknown=false;
        std::set<std::string> modeled;
        for(const auto& d:drivers) {modeled.insert(d.component->at("id").get<std::string>()+":"+d.fact.at("pin").get<std::string>());unknown|=d.fact.value("drive","")!="open_drain" || d.ground!=out.ground;}
        if(inputs.count(net))for(const auto& in:inputs.at(net)){modeled.insert(in.component->at("id").get<std::string>()+":"+in.fact.at("pin").get<std::string>());unknown|=in.ground!=out.ground;}
        for(const auto& p:snapshot.at("components")) if(Passive(p,"resistor")) {
            const auto a=p["pins"][0].value("net",""),b=p["pins"][1].value("net","");
            if(a!=net && b!=net)continue;
            const auto other=a==net?b:a;const auto resistance=PassiveValue(p.value("value",""),"resistor");
            const json* rail=nullptr;for(const auto& r:rails)if(r["net"]==other && r["reference_net"]==out.ground && r.at("min_v").get<double>()>0)rail=&r;
            if(!rail || !resistance){unknown=true;continue;}
            pullup=true;modeled.insert(p.at("id").get<std::string>()+":"+p["pins"][a==net?0:1].at("number").get<std::string>());
            ev.push_back({{"reference",p.at("reference")},{"resistance_ohm",resistance},{"pullup_requirement",*rail},{"pins",p.at("pins")}});
        }
        unknown|=modeled!=terminals[net];
        const auto status=unknown || !inputs.count(net)?"needs_investigation":pullup?"supported_match":"supported_violation";
        add("open_drain",status,*out.component,unknown?"Open-drain topology needs review":pullup?"Open-drain pull-up topology is present":"Open-drain pull-up is missing",
            "Looks for a standard resistor to a declared positive supply with the same reference net. A topology match does not establish logic compatibility: sink current, leakage, resistor tolerance, VOL, rise time and power-off behavior still require review. Unmodeled terminals and external pull-ups are unresolved.",ev,net,out.fact.value("id",""));
    }
    for(const auto& [net,receivers]:inputs) for(const auto& in:receivers)
    {
        const auto& c=*in.component; auto ev=json::array({evidence(in.source,in.fact)});
        auto drivers=outputs.find(net);
        std::set<std::string> modeled;
        for(const auto& p:receivers) modeled.insert(p.component->at("id").get<std::string>()+":"+p.fact.at("pin").get<std::string>());
        if(drivers!=outputs.end()) for(const auto& p:drivers->second) modeled.insert(p.component->at("id").get<std::string>()+":"+p.fact.at("pin").get<std::string>());
        if(drivers==outputs.end() || drivers->second.size()!=1 || modeled!=terminals[net])
        {add("interface","needs_investigation",c,"Interface topology needs review","Requires one modeled driver and approved records for every terminal on this direct net. External connectors, additional drivers, passive networks and level shifters require further analysis.",ev,net,in.fact.value("id",""));continue;}
        const auto& out=drivers->second.front(); ev.push_back(evidence(out.source,out.fact));
        if(out.ground!=in.ground || out.fact.value("drive","")!="push_pull")
        {add("interface","not_checked",c,"Interface cannot use this check","Only direct push-pull connections with the same reference net are supported; open-drain and separate ground domains need another method.",ev,net,in.fact.value("id",""));continue;}
        const bool voltage=out.fact.at("min_v").get<double>()>=in.fact.at("min_v").get<double>() && out.fact.at("max_v").get<double>()<=in.fact.at("max_v").get<double>();
        const bool logic=in.fact.contains("vil_max_v") && out.fact.contains("vol_max_v");
        const bool thresholds=logic && out.fact.at("vol_max_v").get<double>()<=in.fact.at("vil_max_v").get<double>() && out.fact.at("voh_min_v").get<double>()>=in.fact.at("vih_min_v").get<double>();
        const auto status=!voltage || (logic && !thresholds)?"supported_violation":logic?"supported_match":"not_checked";
        add("interface",status,c,!voltage?"Driver voltage exceeds receiver operating range":logic&&!thresholds?"Guaranteed logic levels are incompatible":logic?"Voltage and logic limits are compatible":"Logic thresholds are missing",
            "Driver "+out.component->at("reference").get<std::string>()+" pin "+out.fact.at("pin").get<std::string>()+" → receiver pin "+in.fact.at("pin").get<std::string>()+". Compares voltage envelopes and guaranteed VOL/VOH with VIL/VIH. Timing, power-off states, load, overshoot and signal integrity remain outside this check. Both bound operating contexts must apply simultaneously.",ev,net,in.fact.value("id",""));
    }
    // Outputs without any modeled receiver are not silently presented as reviewed.
    for(const auto& [net,drivers]:outputs) if(!inputs.count(net)) for(const auto& out:drivers)
        add("interface","not_checked",*out.component,"No modeled receiver","Bind and approve receiver specifications before checking this output.",json::array({evidence(out.source,out.fact)}),net,out.fact.value("id",""));
    json counts={{"supported_violation",0},{"supported_match",0},{"needs_investigation",0},{"not_checked",0},{"accepted_exceptions",0}};
    const auto exceptions=state.value("exceptions",json::object());
    for(auto& f:findings)
    {
        counts[f.at("status").get<std::string>()]=counts[f.at("status").get<std::string>()].get<int>()+1;
        const auto id=f.at("id").get<std::string>();
        if(exceptions.contains(id) && exceptions[id].value("fingerprint","")==report["fingerprint"].get<std::string>())
        {f["exception"]=exceptions[id];counts["accepted_exceptions"]=counts["accepted_exceptions"].get<int>()+1;}
    }
    report["counts"]=counts;
    json coverage={{"components",0},{"with_confirmed_evidence",0},{"with_supported_checks",0}};
    for(const auto& c:snapshot.at("components"))if(!c.value("excluded",false)) {
        coverage["components"]=coverage["components"].get<int>()+1;
        const auto id=c.at("id").get<std::string>();bool electrical=false;
        for(const auto& f:findings)if(f["component_id"]==id && f["rule"]!="pin_mapping" && (f["status"]=="supported_match" || f["status"]=="supported_violation"))electrical=true;
        if(electrical)coverage["with_supported_checks"]=coverage["with_supported_checks"].get<int>()+1;
        if(bindings.contains(id)) {
            const auto& b=bindings.at(id);const auto sid=b.value("source_id","");
            if(sources.contains(sid) && !sources.at(sid).contains("error") && !sources.at(sid).value("archived",false) && b.value("identity","")==c.at("identity").get<std::string>() && b.value("source_digest","")==SourceDigest(sources.at(sid))) {
                bool approved=false;for(const auto& f:sources.at(sid).value("specifications",json::array()))approved|=f.value("approved",false);
                if(approved)coverage["with_confirmed_evidence"]=coverage["with_confirmed_evidence"].get<int>()+1;
            }
        }
    }
    report["coverage"]=coverage;return report;
}
inline std::string Markdown(const json& report)
{
    // Escape raw source text so exported Markdown cannot introduce HTML or links.
    auto safe=[](const std::string& s){std::string o;for(char c:s){if(c=='<' )o+="&lt;";else if(c=='>')o+="&gt;";else {if(std::string("\\`*_[]#!|").find(c)!=std::string::npos)o+='\\';o+=c;}}return o;};
    std::string out="# Inductive circuit review\n\nGenerated: "+report.value("created_at","")+"\n\nFingerprint: `"+report.at("fingerprint").get<std::string>()+"`\n\n"+report.at("scope").get<std::string>()+"\n\n";
    for(const auto& f:report.at("findings"))
    {
        out+="## "+safe(f.at("reference"))+": "+safe(f.at("title"))+"\n\nStatus: "+f.at("status").get<std::string>()+"\n\n"+safe(f.at("detail"))+"\n\n";
        for(const auto& e:f.at("evidence")) out+="Evidence: "+safe(e.dump())+"\n\n";
        if(f.contains("exception")) out+="Accepted exception: "+safe(f["exception"].dump())+"\n\n";
    }
    return out;
}
}
#endif
