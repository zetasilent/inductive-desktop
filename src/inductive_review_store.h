#ifndef INDUCTIVE_REVIEW_STORE_H
#define INDUCTIVE_REVIEW_STORE_H
#include "inductive_parts.h"
#include <wx/datetime.h>
#include <memory>
#include <set>
namespace INDUCTIVE_REVIEW
{
inline std::string Now() {return wxDateTime::UNow().ToUTC().FormatISOCombined('T').ToStdString()+"Z";}
class STORE
{
    wxString root;
public:
    explicit STORE(const wxString& project)
    {
        wxFileName file(project);
        if(!file.IsAbsolute() || !file.FileExists()) throw std::runtime_error("Save the project before using circuit review.");
        root=file.GetPathWithSep()+file.GetName()+".inductive-review";
        if(std::filesystem::is_symlink(root.ToStdString())) throw std::runtime_error("Cannot use a linked review folder.");
    }
    json Get() const
    {
        const auto path=root+"/state.json";
        if(!wxFileExists(path)) return {{"version",1},{"revision",0},{"rails",json::array()},{"bindings",json::object()},{"exceptions",json::object()},{"audit",json::array()}};
        auto state=json::parse(INDUCTIVE_PARTS::Read(path,4*1024*1024));
        if(state.value("version",0)!=1 || !state.at("revision").is_number_integer()) throw std::runtime_error("Unsupported circuit review state.");
        return state;
    }
    template<class Change> json Update(int revision,const std::string& action,Change change)
    {
        if(!wxFileName::Mkdir(root,0700,wxPATH_MKDIR_FULL) && !wxDirExists(root)) throw std::runtime_error("Could not create project review folder.");
        INDUCTIVE_PARTS::LOCK lock(root); auto state=Get();
        if(state.value("revision",0)!=revision) throw std::runtime_error("Review configuration changed in another window. Refresh and try again.");
        const auto before=Hash(state.dump());change(state);state["revision"]=revision+1;
        auto& audit=state["audit"];audit.push_back({{"at",Now()},{"action",action},{"previous_sha256",before},{"revision",revision+1}});
        if(audit.size()>200) audit.erase(audit.begin(),audit.begin()+(audit.size()-200));
        const auto serialized=state.dump(2);
        if(serialized.size()>4*1024*1024) throw std::runtime_error("Review state is too large. Reduce the review scope.");
        INDUCTIVE_PARTS::Write(root+"/state.json",serialized);return state;
    }
    wxString Export(const json& report) const
    {
        if(!wxFileName::Mkdir(root,0700,wxPATH_MKDIR_FULL) && !wxDirExists(root)) throw std::runtime_error("Could not create review folder.");
        const auto base=root+"/report-"+KIID().AsString();
        INDUCTIVE_PARTS::Write(base+".json",report.dump(2));
        INDUCTIVE_PARTS::Write(base+".md",Markdown(report));return base+".md";
    }
};
// The change journal is independent of review revisions. Both editor windows read
// the same atomic file; stale requests cannot advance another window's change.
class CHANGE_STORE
{
    wxString root;
    wxString projectFile;
public:
    // Hold across a complete native step, separately from atomic journal writes.
    // A second editor fails promptly; a crashed process releases the OS lock.
    class STEP_LOCK
    {
#ifdef __WXMSW__
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED region{};
#else
        int fd = -1;
#endif
    public:
        explicit STEP_LOCK(const wxString& root)
        {
#ifdef __WXMSW__
            handle=CreateFileW((root+"/.step.lock").wc_str(),GENERIC_READ|GENERIC_WRITE,
                FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Could not lock the engineering workflow.");
            if(!LockFileEx(handle,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&region)) {
                CloseHandle(handle);throw std::runtime_error("An engineering step is running in another window. Wait and refresh.");
            }
#else
            fd=::open((root+"/.step.lock").ToStdString().c_str(),O_CREAT|O_RDWR|O_NOFOLLOW,0600);
            if(fd<0) throw std::runtime_error("Could not lock the engineering workflow.");
            if(::flock(fd,LOCK_EX|LOCK_NB)!=0) {
                ::close(fd);throw std::runtime_error("An engineering step is running in another window. Wait and refresh.");
            }
#endif
        }
        ~STEP_LOCK()
        {
#ifdef __WXMSW__
            UnlockFileEx(handle,0,1,0,&region);CloseHandle(handle);
#else
            ::flock(fd,LOCK_UN);::close(fd);
#endif
        }
        STEP_LOCK(const STEP_LOCK&)=delete;
    };
    explicit CHANGE_STORE(const wxString& project)
    {
        projectFile=project;
        wxFileName file(project);
        if(!file.IsAbsolute() || !file.FileExists()) throw std::runtime_error("Save this project before making an engineering change.");
        root=file.GetPathWithSep()+file.GetName()+".inductive-changes";
        if(std::filesystem::is_symlink(root.ToStdString())) throw std::runtime_error("Cannot use a linked change folder.");
    }
    std::unique_ptr<STEP_LOCK> BeginStep()
    {
        if(!wxFileName::Mkdir(root,0700,wxPATH_MKDIR_FULL) && !wxDirExists(root)) throw std::runtime_error("Could not create change journal.");
        return std::make_unique<STEP_LOCK>(root);
    }
    json Get() const
    {
        if(!wxFileExists(root+"/current.json")) return {{"version",1},{"revision",0}};
        auto state=json::parse(INDUCTIVE_PARTS::Read(root+"/current.json",4*1024*1024));
        if(state.value("version",0)!=1 || !state.at("revision").is_number_integer()) throw std::runtime_error("Unsupported engineering change journal.");
        return state;
    }
    template<class Change> json Update(int revision,Change change)
    {
        if(!wxFileName::Mkdir(root,0700,wxPATH_MKDIR_FULL) && !wxDirExists(root)) throw std::runtime_error("Could not create change journal.");
        INDUCTIVE_PARTS::LOCK lock(root);auto state=Get();
        if(state.value("revision",0)!=revision) throw std::runtime_error("Engineering change updated in another window. Refresh before continuing.");
        const auto previous=state;
        change(state);state["revision"]=revision+1;state["updated_at"]=Now();
        if(state.dump().size()>4*1024*1024) throw std::runtime_error("Engineering change journal is too large.");
        // Archive the previous change before a new request replaces current.json.
        const auto oldId=previous.value("id","");
        if(!oldId.empty() && oldId!=state.value("id","") && INDUCTIVE_PARTS::ValidId(oldId))
            INDUCTIVE_PARTS::Write(root+"/change-"+wxString::FromUTF8(oldId)+".json",previous.dump(2));
        INDUCTIVE_PARTS::Write(root+"/current.json",state.dump(2));return state;
    }
    json History() const
    {
        json entries=json::array();
        auto append=[&](json entry) {
            if(entry.value("id","").empty())return;
            if(entry.contains("summary"))entry["summary"].erase("visual");
            entries.push_back(entry);
        };
        append(Get());
        if(std::filesystem::exists(root.ToStdString()))for(const auto& f:std::filesystem::directory_iterator(root.ToStdString())) {
            const auto name=f.path().filename().string();
            if(f.is_symlink() || !f.is_regular_file() || name.rfind("change-",0)!=0 || f.path().extension()!=".json")continue;
            try {append(json::parse(INDUCTIVE_PARTS::Read(wxString::FromUTF8(f.path().string()),4*1024*1024)));}catch(const std::exception&){}
        }
        std::sort(entries.begin(),entries.end(),[](const json& a,const json& b){return a.value("updated_at","")>b.value("updated_at","");});
        if(entries.size()>100)entries.erase(entries.begin()+100,entries.end());
        return entries;
    }
    json Recover(const std::string& checkpoint) const
    {
        namespace fs=std::filesystem;
        // Only archives issued by this project's native workflow can be opened.
        bool known=false;
        for(const auto& entry:History()) {
            for(const auto* key:{"checkpoint","pcb_checkpoint","manual_checkpoint"})if(entry.value(key,"")==checkpoint)known=true;
            for(const auto& point:entry.value("recovery_points",json::array()))if(point.value("path","")==checkpoint)known=true;
        }
        const fs::path archive(checkpoint), base=fs::path(projectFile.ToStdString()).parent_path();
        if(!known || archive.parent_path()!=fs::path(root.ToStdString()) || fs::is_symlink(archive))
            throw std::runtime_error("Choose a recovery checkpoint from this project's history.");
        const auto data=json::parse(INDUCTIVE_PARTS::Read(wxString::FromUTF8(checkpoint),32*1024*1024));
        if(data.value("version",0)!=1 || !data.at("files").is_array() || data.at("files").empty())throw std::runtime_error("Unsupported recovery checkpoint.");
        auto files=data.at("files");
        for(const auto& support:data.value("support_files",json::array()))files.push_back(support);
        std::set<fs::path> unique;
        for(const auto& f:files) {
            const fs::path path(f.at("path").get<std::string>());const auto relative=path.lexically_relative(base);
            if(!path.is_absolute() || relative.empty() || relative.is_absolute() || !f.at("content").is_string()
                || !unique.insert(relative).second || std::find(relative.begin(),relative.end(),"..")!=relative.end())
                throw std::runtime_error("This checkpoint has external or invalid paths. Use the archive extraction tool to inspect it.");
        }
        const fs::path destination=fs::path(root.ToStdString())/("recovered-"+KIID().AsString().ToStdString());
        if(!fs::create_directory(destination))throw std::runtime_error("Could not create a recovery folder.");
        json manifest={{"checkpoint",checkpoint},{"created_at",Now()},{"files",json::array()},
            {"scope","Native documents before the selected step; companion files as captured at that time. Inspect libraries and rerun checks."}};
        const auto oldPrefix=json(base.string()+"/").dump();
        const auto newPrefix=json(destination.string()+"/").dump();
        for(const auto& f:files) {
            const fs::path original(f.at("path").get<std::string>());const auto target=destination/original.lexically_relative(base);
            fs::create_directories(target.parent_path());std::string content=f.at("content");
            // Rebase absolute references to files within the original project.
            const auto from=oldPrefix.substr(0,oldPrefix.size()-1),to=newPrefix.substr(0,newPrefix.size()-1);
            size_t offset=0;while((offset=content.find(from,offset))!=std::string::npos){content.replace(offset,from.size(),to);offset+=to.size();}
            INDUCTIVE_PARTS::Write(wxString::FromUTF8(target.string()),content);
            manifest["files"].push_back({{"original",original.string()},{"recovered",target.string()}});
        }
        const auto recoveredProject=destination/fs::path(projectFile.ToStdString()).filename();
        if(!fs::exists(recoveredProject))INDUCTIVE_PARTS::Write(wxString::FromUTF8(recoveredProject.string()),"{}\n");
        INDUCTIVE_PARTS::Write(wxString::FromUTF8((destination/"recovery-manifest.json").string()),manifest.dump(2));
        return {{"project",recoveredProject.string()},{"folder",destination.string()},{"checkpoint",checkpoint},
            {"message",data.contains("support_files")?"Recovery copy created. Inspect the recovered revision and rerun checks.":"Recovered native documents from an older archive. Project settings and libraries were not captured; inspect them before editing."}};
    }
    wxString Checkpoint(const json& files)
    {
        if(files.empty() || !files.is_array()) throw std::runtime_error("No native documents to checkpoint.");
        std::set<std::string> paths;
        for(const auto& file:files) {
            wxFileName path(wxString::FromUTF8(file.at("path").get<std::string>()));
            if(!path.IsAbsolute() || (path.GetExt()!="kicad_sch" && path.GetExt()!="kicad_pcb")
               || !file.at("content").is_string() || !paths.insert(path.GetFullPath().ToStdString()).second)
                throw std::runtime_error("Save every native sheet or board to a distinct KiCad file before applying this change.");
        }
        if(!wxFileName::Mkdir(root,0700,wxPATH_MKDIR_FULL) && !wxDirExists(root)) throw std::runtime_error("Could not create change checkpoint.");
        auto path=root+"/checkpoint-"+KIID().AsString()+".json";
        json support=json::array();
        namespace fs=std::filesystem;
        const fs::path base=fs::path(projectFile.ToStdString()).parent_path();
        size_t bytes=files.dump().size();
        auto capture=[&](const fs::path& file) {
            if(paths.count(file.string()) || !fs::is_regular_file(file) || fs::is_symlink(file))return;
            const auto content=INDUCTIVE_PARTS::Read(wxString::FromUTF8(file.string()),32*1024*1024);
            bytes+=content.size();if(bytes>30*1024*1024 || support.size()>1000)throw std::runtime_error("Recovery dependencies exceed 30 MiB or 1,000 files.");
            support.push_back({{"path",file.string()},{"content",content}});paths.insert(file.string());
        };
        capture(fs::path(projectFile.ToStdString()));
        auto counterpart=fs::path(projectFile.ToStdString());counterpart.replace_extension(files.front().at("path").get<std::string>().find(".kicad_pcb")!=std::string::npos?".kicad_sch":".kicad_pcb");capture(counterpart);
        size_t visited=0;
        for(auto it=fs::recursive_directory_iterator(base,fs::directory_options::skip_permission_denied);it!=fs::recursive_directory_iterator();++it) {
            if(++visited>50000)throw std::runtime_error("The project folder is too large to checkpoint. Keep each design in its own folder.");
            const auto file=it->path();const auto name=file.filename().string();
            if(it->is_symlink()){it.disable_recursion_pending();continue;}
            if(it->is_directory()) {if(name.empty() || name[0]=='.' || name.find(".inductive-")!=std::string::npos || name.find("backups")!=std::string::npos || name=="node_modules" || name=="build" || it.depth()>=12)it.disable_recursion_pending();continue;}
            if(file.extension()==".kicad_sym" || file.extension()==".kicad_mod" || file.extension()==".kicad_sch" || file.extension()==".kicad_pcb" || name=="sym-lib-table" || name=="fp-lib-table")capture(file);
        }
        const auto data=json{{"version",1},{"created_at",Now()},{"files",files},{"support_files",support}}.dump(2);
        if(data.size()>32*1024*1024) throw std::runtime_error("Checkpoint exceeds 32 MiB. Reduce the project scope.");
        INDUCTIVE_PARTS::Write(path,data);return path;
    }
};

// The handoff's publish step, for a netlist published outside it: /pcb, or the
// model's own prepare_pcb after a direct edit. The latest change goes on to the
// PCB only while the sheet is still the one that change produced; otherwise the
// PCB card would describe a stale diff. Null when nothing was handed on, with
// aStale set when an applied change was left behind for that reason.
inline json PublishHandoff(CHANGE_STORE& store,const std::string& fingerprint,const wxString& netlist,
                           const std::string& message,bool& aStale)
{
    aStale=false;
    auto stepLock=store.BeginStep();auto state=store.Get();
    const auto stage=state.value("stage","");
    if(stage!="applied" && stage!="published") return nullptr;
    if(state.value("sch_fingerprint","")!=fingerprint){aStale=true;return nullptr;}
    const auto hash=Hash(INDUCTIVE_PARTS::Read(netlist,16*1024*1024));
    return store.Update(state.at("revision"),[&](auto& j){
        j["stage"]="published";j["netlist_hash"]=hash;
        j["last_step"]={{"action","publish"},{"completed",true}};j["message"]=message;
        if(!j.contains("events"))j["events"]=json::array();
        j["events"].push_back({{"action","publish"},{"ok",true},{"at",Now()},{"message",message}});
        if(j["events"].size()>30)j["events"].erase(j["events"].begin());
    });
}

}
#endif
