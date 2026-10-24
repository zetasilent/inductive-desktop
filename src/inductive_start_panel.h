#ifndef INDUCTIVE_START_PANEL_H
#define INDUCTIVE_START_PANEL_H
#include <kicommon.h>
#include <wx/panel.h>
#include <wx/timer.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <future>
#include <functional>
class wxWebView;

class KICOMMON_API INDUCTIVE_START_PANEL : public wxPanel
{
public:
    using Handler=std::function<bool(const wxString&,bool)>;
    INDUCTIVE_START_PANEL(wxWindow*,Handler,std::function<void()>);
    ~INDUCTIVE_START_PANEL() override;
    bool NeedsOnboarding() const;
    void RememberProject(const wxString&);
private:
    void Message(const std::string&);
    void Deliver(const nlohmann::json&);
    void Refresh();
    void Poll(wxTimerEvent&);
    wxWebView* web=nullptr;
    Handler open;
    std::function<void()> create;
    wxTimer timer;
    std::atomic<bool> cancel{false};
    bool busy=false;   ///< A message is being handled (a file dialog may be open).
    std::future<nlohmann::json> scan;
    nlohmann::json candidates=nlohmann::json::array();
    wxString libraryRoot;
};
#endif
