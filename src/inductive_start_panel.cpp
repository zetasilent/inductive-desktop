#include <widgets/inductive_start_panel.h>
#include <widgets/inductive_panel.h>
#include <widgets/inductive_projects.h>
#include <widgets/inductive_updates.h>
#include <wx/webview.h>
#include <wx/filesys.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>

using namespace INDUCTIVE_PROJECTS;

INDUCTIVE_START_PANEL::INDUCTIVE_START_PANEL(wxWindow* parent,Handler handler,std::function<void()> newProject):
    wxPanel(parent),open(std::move(handler)),create(std::move(newProject)),timer(this)
{
#ifdef __WXMAC__
    libraryRoot=wxFileName::GetHomeDir()+"/Library/Application Support/Inductive";
#else
    libraryRoot=wxStandardPaths::Get().GetUserDataDir()+"/Inductive";
#endif
    SetBackgroundColour(*wxBLACK);
    auto* sizer=new wxBoxSizer(wxVERTICAL);
    web=wxWebView::New(this,wxID_ANY);
    if(web) {
        web->EnableContextMenu(false);
        web->AddScriptMessageHandler("inductiveHost");
        // Handled after WebKit's callback has returned, never inside it. A folder dialog
        // opened from within the callback ran a modal loop there; a second message from
        // the page re-entered WebKit's handler underneath it and crashed the app
        // (SIGSEGV in WebViewScriptMessageHandler, 2026-10-18).
        web->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,[this](wxWebViewEvent& e){
            const std::string text(e.GetString().utf8_str());
            CallAfter([this,text]{Message(text);});
        });
        web->Bind(wxEVT_WEBVIEW_LOADED,[this](wxWebViewEvent&){try{Refresh();}catch(const std::exception& e){Deliver({{"type","home_error"},{"message",e.what()}});}});
        sizer->Add(web,1,wxEXPAND);
        web->LoadURL(wxFileSystem::FileNameToURL(wxFileName(INDUCTIVE_PANEL::WebDir(),"home.html")));
    } else sizer->Add(new wxStaticText(this,wxID_ANY,"Home needs the system web view. Open projects using the File menu."),0,wxALL,24);
    SetSizer(sizer);
    // A release build asks for updates as it starts and requires a newer one.
    CallAfter([]{INDUCTIVE_UPDATES::CheckAtLaunch();});
    Bind(wxEVT_TIMER,&INDUCTIVE_START_PANEL::Poll,this);
    // KiCad 10's project manager builds this page while its notebook is frozen and shows
    // the Editors page first; the web view then stayed blank the first time Home was
    // chosen -- no Open or New button to click -- until another tab was chosen and back.
    // Lay the page out and repaint it whenever it is shown.
    Bind(wxEVT_SHOW,[this](wxShowEvent& e){
        e.Skip();
        if(e.IsShown())CallAfter([this]{Layout();if(web){web->SetSize(GetClientSize());web->Refresh();web->Update();}});
    });
}

INDUCTIVE_START_PANEL::~INDUCTIVE_START_PANEL()
{
    timer.Stop();cancel=true;
    if(scan.valid())scan.wait(); // Worker owns no UI pointers or callbacks.
}

void INDUCTIVE_START_PANEL::Deliver(const json& data)
{
    if(web)web->RunScriptAsync("window.inductiveDeliver("+wxString::FromUTF8(data.dump())+");");
}

void INDUCTIVE_START_PANEL::Refresh()
{
    try{LIBRARY(libraryRoot).ForgetVanishedTemporary();}catch(...){}   // never in the way of Home
    auto data=LIBRARY(libraryRoot).Get();data["type"]="home_state";
    for(auto& project:data["projects"])project["missing"]=!wxFileExists(wxString::FromUTF8(project.at("path").get<std::string>()));
    Deliver(data);
}

bool INDUCTIVE_START_PANEL::NeedsOnboarding() const
{
    try{return !LIBRARY(libraryRoot).Get().value("onboarded",false);}catch(...){return true;}
}

void INDUCTIVE_START_PANEL::RememberProject(const wxString& path)
{
    try{LIBRARY(libraryRoot).Save(json::array({Describe(path.ToStdString())}));Refresh();}
    catch(const std::exception& e){Deliver({{"type","home_error"},{"message",e.what()}});}
}

void INDUCTIVE_START_PANEL::Poll(wxTimerEvent&)
{
    if(!scan.valid() || scan.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return;
    timer.Stop();
    try {
        auto result=scan.get();candidates=result.at("projects");result["type"]="home_scan";Deliver(result);
    } catch(const std::exception& e) {Deliver({{"type","home_error"},{"message",e.what()}});}
}

void INDUCTIVE_START_PANEL::Message(const std::string& text)
{
    // One at a time: while a dialog is open its loop can deliver the next message, and
    // a second dialog on top of the first is never what was meant. Cancel always gets in.
    if(busy && text.find("\"home_cancel\"")==std::string::npos)return;
    struct BUSY{bool& b;explicit BUSY(bool& f):b(f){b=true;}~BUSY(){b=false;}} guard(busy);
    try {
        if(text.size()>2*1024*1024)throw std::runtime_error("The request is too large.");
        const auto msg=json::parse(text);const auto type=msg.at("type").get<std::string>();
        if(type=="home_cancel") {cancel=true;return;}
        if(scan.valid())throw std::runtime_error("Wait for the folder search to finish or cancel it.");
        if(type=="home_refresh") {Refresh();return;}
        if(type=="home_complete") {LIBRARY(libraryRoot).Save(json::array(),true);Refresh();return;}
        if(type=="home_scan") {
            wxDirDialog choose(this,"Choose a folder containing your KiCad projects",wxEmptyString,wxDD_DIR_MUST_EXIST);
            if(choose.ShowModal()!=wxID_OK){Deliver({{"type","home_cancelled"}});return;}
            const auto folder=choose.GetPath().ToStdString();cancel=false;candidates=json::array();
            Deliver({{"type","home_scanning"},{"root",folder}});
            scan=std::async(std::launch::async,[this,folder]{return Scan(folder,cancel);});timer.Start(100);return;
        }
        if(type=="home_import") {
            std::set<std::string> requested;
            for(const auto& path:msg.at("paths"))requested.insert(path.get<std::string>());
            json selected=json::array();for(const auto& p:candidates)if(requested.erase(p.at("path")))selected.push_back(p);
            if(!requested.empty() || selected.empty())throw std::runtime_error("Select projects from the latest folder search.");
            LIBRARY(libraryRoot).Save(selected,true);candidates=json::array();Refresh();
            Deliver({{"type","home_imported"},{"count",selected.size()}});return;
        }
        if(type=="home_new") {LIBRARY(libraryRoot).Save(json::array(),true);Refresh();create();return;}
        if(type=="home_open") {
            std::string path=msg.value("path","");
            if(path.empty()) {
                wxFileDialog choose(this,"Open a KiCad project",wxEmptyString,wxEmptyString,
                    "KiCad projects (*.kicad_pro;*.pro)|*.kicad_pro;*.pro",wxFD_OPEN|wxFD_FILE_MUST_EXIST);
                if(choose.ShowModal()!=wxID_OK){Deliver({{"type","home_cancelled"}});return;}
                path=choose.GetPath().ToStdString();
            } else {
                const auto library=LIBRARY(libraryRoot).Get();
                bool found=false;for(const auto& p:library.at("projects"))if(p.at("path")==path)found=true;
                if(!found)throw std::runtime_error("This project is not in your library. Use Open project to locate it.");
            }
            auto entry=Describe(path);
            if(!open(wxString::FromUTF8(entry.at("path").get<std::string>()),false))throw std::runtime_error("The project was not opened.");
            LIBRARY(libraryRoot).Save(json::array({entry}),true);Refresh();return;
        }
        if(type=="home_starter") {
            const auto id=msg.at("id").get<std::string>();
            if(id!="led" && id!="buck" && id!="motor")throw std::runtime_error("Unknown starter circuit.");
            const auto name=msg.value("name",id);
            if(name.empty() || name.size()>64 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
                throw std::runtime_error("Use 1–64 letters, numbers, dashes or underscores for the project name.");
            wxDirDialog choose(this,"Choose where to create the starter project",wxEmptyString,wxDD_DIR_MUST_EXIST);
            if(choose.ShowModal()!=wxID_OK){Deliver({{"type","home_cancelled"}});return;}
            const auto destination=fs::path(choose.GetPath().ToStdString())/name;
            const auto assets=fs::path(INDUCTIVE_PANEL::WebDir().ToStdString())/"starters"/id;
            auto schematic=INDUCTIVE_PARTS::Read(wxString::FromUTF8((assets/(id+".kicad_sch")).string()),4*1024*1024);
            const auto oldProject="(project \""+id+"\"";
            const auto newProject="(project \""+name+"\"";
            size_t pos=0;while((pos=schematic.find(oldProject,pos))!=std::string::npos){schematic.replace(pos,oldProject.size(),newProject);pos+=newProject.size();}
            const auto guide=INDUCTIVE_PARTS::Read(wxString::FromUTF8((assets/"README.md").string()));
            if(!fs::create_directory(destination))throw std::runtime_error("A folder with this name already exists. Choose a different project name.");
            // The directory was created exclusively. A partial failure is retained for inspection.
            INDUCTIVE_PARTS::Write(wxString::FromUTF8((destination/(name+".kicad_sch")).string()),schematic);
            INDUCTIVE_PARTS::Write(wxString::FromUTF8((destination/(name+".kicad_pro")).string()),"{}\n");
            INDUCTIVE_PARTS::Write(wxString::FromUTF8((destination/"START-HERE.md").string()),guide);
            auto project=Describe(destination/(name+".kicad_pro"));
            LIBRARY(libraryRoot).Save(json::array({project}),true);Refresh();
            if(!open(wxString::FromUTF8(project.at("path").get<std::string>()),true))throw std::runtime_error("Starter saved. Open it from your project library.");
            return;
        }
    } catch(const std::exception& e) {Deliver({{"type","home_error"},{"message",e.what()}});}
      catch(...) {Deliver({{"type","home_error"},{"message","This action could not finish. Your existing projects are unchanged."}});}
}
