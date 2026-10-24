#ifndef INDUCTIVE_PROJECTS_H
#define INDUCTIVE_PROJECTS_H

#include "inductive_parts.h"
#include <atomic>
#include <fstream>

// Project discovery only reads user-selected folders. Import stores canonical
// paths; it never relocates a project or rewrites its library references.
namespace INDUCTIVE_PROJECTS
{
using json = nlohmann::json;
namespace fs = std::filesystem;

inline bool SkipDirectory(const fs::path& path)
{
    const auto name=path.filename().string();
    return name.empty() || name[0]=='.' || name=="node_modules" || name=="__MACOSX"
        || name=="backups" || name=="recovery" || name=="build"
        || name.find("-backups")!=std::string::npos
        || name.find(".inductive-")!=std::string::npos;
}

inline json Describe(const fs::path& source)
{
    const auto path=fs::canonical(source);
    if(!fs::is_regular_file(path)) throw std::runtime_error("Project file is unavailable.");
    const auto ext=path.extension().string();
    if(ext!=".kicad_pro" && ext!=".pro") throw std::runtime_error("Choose a KiCad project file.");
    const auto text=INDUCTIVE_PARTS::Read(wxString::FromUTF8(path.string()),4*1024*1024);
    if(ext==".kicad_pro") {
        const auto data=json::parse(text,nullptr,false);
        if(!data.is_object()) throw std::runtime_error("Invalid KiCad project file.");
    } else if(text.find("[eeschema")==std::string::npos && text.find("[pcbnew")==std::string::npos
              && text.find("[general]")==std::string::npos)
        throw std::runtime_error("This .pro file is not a KiCad project.");
    auto base=path; base.replace_extension();
    return {{"path",path.string()},{"name",path.stem().string()},
        {"legacy",ext==".pro"},{"schematic",fs::exists(base.string()+".kicad_sch") || fs::exists(base.string()+".sch")},
        {"pcb",fs::exists(base.string()+".kicad_pcb") || fs::exists(base.string()+".brd")}};
}

inline json Scan(const fs::path& folder,const std::atomic<bool>& cancel)
{
    const auto root=fs::canonical(folder);
    if(!fs::is_directory(root)) throw std::runtime_error("Choose a folder to search.");
    json projects=json::array(); size_t visited=0, skipped=0; bool limited=false;
    std::set<std::string> seen;
    std::error_code ec;
    fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;
    if(ec) throw std::runtime_error("This folder cannot be read.");
    while(it!=end && !cancel) {
        const auto path=it->path();
        if(++visited>200000 || projects.size()>=10000) {limited=true;break;}
        const auto status=it->symlink_status(ec);
        if(ec) {++skipped;ec.clear();}
        else if(fs::is_symlink(status)) it.disable_recursion_pending();
        else if(fs::is_directory(status)) {
            if(SkipDirectory(path) || it.depth()>=32) it.disable_recursion_pending();
        } else if(fs::is_regular_file(status) && (path.extension()==".kicad_pro" || path.extension()==".pro")) {
            auto modern=path;modern.replace_extension(".kicad_pro");
            if(path.extension()!=".pro" || !fs::exists(modern)) {
                try {auto entry=Describe(path);if(seen.insert(entry.at("path")).second)projects.push_back(entry);}
                catch(const std::exception&) {++skipped;}
            }
        }
        it.increment(ec);if(ec){++skipped;ec.clear();}
    }
    std::sort(projects.begin(),projects.end(),[](const json& a,const json& b){return a.at("path")<b.at("path");});
    return {{"projects",projects},{"root",root.string()},{"cancelled",cancel.load()},
        {"limited",limited},{"skipped",skipped},{"visited",visited}};
}

class LIBRARY
{
    wxString root;
public:
    explicit LIBRARY(const wxString& directory):root(directory) {}
    json Get() const
    {
        const auto path=root+"/projects.json";
        if(!wxFileExists(path))return {{"version",1},{"onboarded",false},{"projects",json::array()}};
        auto data=json::parse(INDUCTIVE_PARTS::Read(path,8*1024*1024));
        if(data.value("version",0)!=1 || !data.at("projects").is_array())throw std::runtime_error("Unsupported project library.");
        return data;
    }
    json Save(const json& additions,bool complete=false)
    {
        if(!wxFileName::Mkdir(root,0700,wxPATH_MKDIR_FULL) && !wxDirExists(root))throw std::runtime_error("Cannot save the project library.");
        INDUCTIVE_PARTS::LOCK lock(root);auto data=Get();std::set<std::string> paths;
        for(const auto& p:data["projects"])paths.insert(p.at("path"));
        for(const auto& p:additions) {
            auto entry=Describe(p.at("path").get<std::string>());
            if(paths.insert(entry.at("path")).second)data["projects"].push_back(entry);
        }
        if(data["projects"].size()>10000)throw std::runtime_error("The project library supports 10,000 projects.");
        if(complete)data["onboarded"]=true;
        INDUCTIVE_PARTS::Write(root+"/projects.json",data.dump(2));return data;
    }
    /// Forget projects that lived in a temporary folder and are gone: they cannot come
    /// back, and a list of "Missing" rows is what Home opened with after test runs. A
    /// project anywhere else stays listed when missing -- it may be on a drive that is out.
    int ForgetVanishedTemporary()
    {
        if(!wxFileExists(root+"/projects.json"))return 0;
        INDUCTIVE_PARTS::LOCK lock(root);auto data=Get();json kept=json::array();int dropped=0;
        for(const auto& p:data["projects"]) {
            const auto path=p.value("path",std::string());
            const bool temporary=path.rfind("/private/tmp/",0)==0 || path.rfind("/tmp/",0)==0
                                 || path.rfind("/private/var/folders/",0)==0 || path.rfind("/var/folders/",0)==0;
            if(temporary && !wxFileExists(wxString::FromUTF8(path)))++dropped;else kept.push_back(p);
        }
        if(dropped){data["projects"]=kept;INDUCTIVE_PARTS::Write(root+"/projects.json",data.dump(2));}
        return dropped;
    }
};
}
#endif
