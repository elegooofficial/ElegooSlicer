#include "PrinterManagerView.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r_version.h"
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/dc.h>
#include <wx/bitmap.h>
#include <wx/aui/auibook.h>
#include <wx/aui/aui.h>
#include <wx/aui/auibar.h>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>
#include "slic3r/Utils/PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include <map>
#include <set>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include "slic3r/Utils/WebviewIPCManager.h"
#include <boost/nowide/fstream.hpp>
#include <mutex>
#include "slic3r/Utils/Elegoo/PrinterNetworkEvent.hpp"
#include "slic3r/Utils/Elegoo/UserNetworkManager.hpp"
#include "slic3r/Utils/Elegoo/PrinterManager.hpp"
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include "TelemetryEvents.hpp"

#define FIRST_TAB_NAME _L("Connected Printer")
#define TAB_MAX_WIDTH 200
#define TAB_MIN_WIDTH 20
#define TAB_PADDING 8
#define TAB_ICON_TEXT_SPACING 8
#define TAB_CLOSE_BUTTON_SIZE 20
#define TAB_CLOSE_BUTTON_MARGIN 6
#define TAB_SEPARATOR_WIDTH 1
#define TAB_HEIGHT 28
#define TAB_BORDER_WIDTH 1

namespace Slic3r {
namespace GUI {

// Static mutex for tab state file operations
static std::mutex s_tabStateMutex;

class TabArt : public wxAuiSimpleTabArt
{
private:
    struct TabMetrics {
        int maxWidth;
        int minWidth;
        int padding;
        int iconTextSpacing;
        int closeButtonSize;
        int closeButtonMargin;
        int separatorWidth;
        int height;
        int borderWidth;
    };

    bool isDarkMode() const {
        return GUI_App::dark_mode();
    }

    int scale(wxWindow* wnd, int value) const {
        return wnd ? wnd->FromDIP(value) : value;
    }

    TabMetrics getMetrics(wxWindow* wnd) const {
        TabMetrics metrics;
        metrics.maxWidth = scale(wnd, TAB_MAX_WIDTH);
        metrics.minWidth = scale(wnd, TAB_MIN_WIDTH);
        metrics.padding = scale(wnd, TAB_PADDING);
        metrics.iconTextSpacing = scale(wnd, TAB_ICON_TEXT_SPACING);
        metrics.closeButtonSize = scale(wnd, TAB_CLOSE_BUTTON_SIZE);
        metrics.closeButtonMargin = scale(wnd, TAB_CLOSE_BUTTON_MARGIN);
        metrics.separatorWidth = std::max(1, scale(wnd, TAB_SEPARATOR_WIDTH));
        metrics.height = scale(wnd, TAB_HEIGHT);
        metrics.borderWidth = std::max(1, 1);
        return metrics;
    }

    wxAuiNotebook* getNotebookFrom(wxWindow* wnd) const {
        wxWindow* current = wnd;
        while (current) {
            if (auto nb = dynamic_cast<wxAuiNotebook*>(current)) {
                return nb;
            }
            current = current->GetParent();
        }
        return nullptr;
    }

    int calculateTabWidth(wxWindow* wnd, bool isFirstTab) const {
        const TabMetrics metrics = getMetrics(wnd);

        if (isFirstTab) {
            return metrics.maxWidth;
        }
        
        wxAuiNotebook* notebook = getNotebookFrom(wnd);
        if (!notebook) {
            return metrics.maxWidth;
        }
        
        const int totalPages = notebook->GetPageCount();
        const int otherPages = totalPages - 1; // Exclude first tab
        
        if (otherPages <= 0) {
            return metrics.maxWidth;
        }
        
        const int totalWidth = notebook->GetClientSize().x;
        const int availableWidth = totalWidth - metrics.maxWidth;
        
        if (availableWidth <= 0) {
            return metrics.minWidth;
        }
        
        const int avgWidth = availableWidth / otherPages;
        return std::max(metrics.minWidth, std::min(metrics.maxWidth, avgWidth));
    }

    wxBitmap getTabIcon(const wxString& caption, wxWindow* wnd) const {
        if (caption == FIRST_TAB_NAME) {
            return create_scaled_bitmap("printer_manager", wnd, 16);
        }

        return create_scaled_bitmap("elegoo_tab", wnd, 12);
    }

    wxSize getBitmapDrawSize(const wxBitmap& bitmap) const {
        if (!bitmap.IsOk()) {
            return wxSize(0, 0);
        }

        return wxSize(static_cast<int>(bitmap.GetScaledWidth()),
                      static_cast<int>(bitmap.GetScaledHeight()));
    }

    void drawTabContent(wxDC& dc,
                        wxWindow* wnd,
                        const wxRect& tab_rect,
                        const wxBitmap& icon,
                        const wxString& text,
                        const wxColour& text_colour,
                        bool isFirstTab) const {
        const TabMetrics metrics = getMetrics(wnd);

        if (wnd) {
            dc.SetFont(wnd->GetFont());
        }

        dc.SetTextForeground(text_colour);
        const wxSize icon_size = getBitmapDrawSize(icon);
        const wxSize text_size = dc.GetTextExtent(text);
        
        // Calculate positions
        const int icon_x = tab_rect.x + metrics.padding;
        const int text_x = icon_x + icon_size.x + (icon.IsOk() ? metrics.iconTextSpacing : 0);
        const int icon_y = tab_rect.y + (tab_rect.height - icon_size.y) / 2;
        const int text_y = tab_rect.y + (tab_rect.height - text_size.y) / 2;
        
        // Draw icon
        if (icon.IsOk()) {
            dc.DrawBitmap(icon, icon_x, icon_y, true);
        }
        
        // Draw text with clipping (no ellipsis)
        const int close_button_space = isFirstTab ? metrics.padding : (metrics.closeButtonSize + metrics.closeButtonMargin * 2);
        const int text_max_width = tab_rect.x + tab_rect.width - text_x - close_button_space;
        
        if (text_max_width > 0) {
            wxRect text_clip(text_x, tab_rect.y, text_max_width, tab_rect.height);
            dc.SetClippingRegion(text_clip);
            dc.DrawText(text, text_x, text_y);
            dc.DestroyClippingRegion();
        }
    }

    void drawCloseButton(wxDC& dc,
                         wxWindow* wnd,
                         const wxRect& tab_rect,
                         int close_button_state,
                         wxRect* out_button_rect) const {
        const TabMetrics metrics = getMetrics(wnd);
        wxRect close_rect(
            tab_rect.x + tab_rect.width - metrics.closeButtonSize - metrics.closeButtonMargin,
            tab_rect.y + (tab_rect.height - metrics.closeButtonSize) / 2,
            metrics.closeButtonSize,
            metrics.closeButtonSize
        );
        
        if (out_button_rect) *out_button_rect = close_rect;
        
        // Simple SVG with color modification
        wxBitmap close_icon = create_scaled_bitmap("topbar_close", wnd, TAB_CLOSE_BUTTON_SIZE);
        if (close_icon.IsOk()) {
            auto close_icon_ = wxBitmap(close_icon.ConvertToImage().Rescale(metrics.closeButtonSize, metrics.closeButtonSize));
            // Change color based on state
            if (close_button_state == wxAUI_BUTTON_STATE_HOVER || close_button_state == wxAUI_BUTTON_STATE_PRESSED) {
                wxImage img = close_icon_.ConvertToImage();
                wxColour new_color = isDarkMode() ? wxColour(255, 120, 120) : wxColour(200, 0, 0);
                
                // Simple color replacement: replace dark pixels with new color
                for (int y = 0; y < img.GetHeight(); y++) {
                    for (int x = 0; x < img.GetWidth(); x++) {
                        if (img.GetAlpha(x, y) > 0) { // Only process non-transparent pixels
                            img.SetRGB(x, y, new_color.Red(), new_color.Green(), new_color.Blue());
                        }
                    }
                }
                close_icon_ = wxBitmap(img);
            }

            const wxSize close_icon_size = getBitmapDrawSize(close_icon_);
            const int icon_x = close_rect.x + (close_rect.width - close_icon_size.x) / 2;
            const int icon_y = close_rect.y + (close_rect.height - close_icon_size.y) / 2;
            dc.DrawBitmap(close_icon_, icon_x, icon_y, true);
        }
    }

    void getColorScheme(wxColour& activeTab, wxColour& inactiveTab, wxColour& hoverTab, wxColour& activeText, 
                       wxColour& inactiveText, wxColour& background, wxColour& border, wxColour &tabHeaderBackground,
                       wxColour& separator) const {
        if (isDarkMode()) {
            // dark mode color scheme
            activeTab = wxColour(107, 107, 107);     
            inactiveTab = wxColour(45, 45, 48);   // inactive tab: light gray
            hoverTab = wxColour(70, 70, 70);      // hover tab: medium gray
            activeText = wxColour(255, 255, 255);    // active tab text: white
            inactiveText = wxColour(200, 200, 200);  // inactive tab text: light gray
            background = wxColour(45, 45, 45);       // tab bar background: dark gray
            border =  wxColour(0, 0, 0);           // border: black
            tabHeaderBackground = wxColour(45, 45, 45);   // tab header background: dark gray
            separator = wxColour(80, 80, 80);        
        } else {
            // light mode color scheme
            activeTab = wxColour(107, 107, 107);     
            inactiveTab = wxColour(56, 68, 70);      // inactive tab: dark gray
            hoverTab = wxColour(80, 90, 95);        // hover tab: lighter gray
            activeText = wxColour(255, 255, 255);    // active tab text: white
            inactiveText = wxColour(200, 200, 200);     // inactive tab text: dark gray
            background = wxColour(250, 250, 250);    // tab bar background: almost white
            border = wxColour(0, 0, 0);  // border: black
            tabHeaderBackground = wxColour(56, 68, 70);
            separator = wxColour(120, 120, 120); 
        }
    }

public:
    TabArt() {
        wxColour activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator;
        getColorScheme(activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator);
        SetColour(inactiveTab);        // inactive tab background color
        SetActiveColour(activeTab);    // active tab background color
    }

    wxAuiTabArt* Clone() override { return new TabArt(*this); }
    
    void DrawTab(wxDC& dc,
                 wxWindow* wnd,
                 const wxAuiNotebookPage& page,
                 const wxRect& in_rect,
                 int close_button_state,
                 wxRect* out_tab_rect,
                 wxRect* out_button_rect,
                 int* x_extent) override
    {
        const bool isFirstTab = (page.caption == FIRST_TAB_NAME);
        const bool isActive = page.active;
        const TabMetrics metrics = getMetrics(wnd);

        // Get color scheme
        wxColour activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator;
        getColorScheme(activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator);
        
        // Calculate tab width
        int tabWidth = calculateTabWidth(wnd, isFirstTab);
  
        wxRect tab_rect = in_rect;
        tab_rect.y = in_rect.y + metrics.borderWidth;
        tab_rect.width = std::max(0, tabWidth - metrics.separatorWidth);
        tab_rect.height = std::max(0, tab_rect.height - metrics.borderWidth);
        // Get icon and text
        wxBitmap icon = getTabIcon(page.caption, wnd);
        wxString text = page.caption;

        // Check if mouse is over this tab
        wxPoint mousePos = wnd->ScreenToClient(wxGetMousePosition());
        bool mouseOverTab = tab_rect.Contains(mousePos);
        
        // Select colors based on active state and hover state
        wxColour tab_colour;
        if (isActive) {
            tab_colour = activeTab;
        } else if (mouseOverTab) {
            tab_colour = hoverTab;
        } else {
            tab_colour = inactiveTab;
        }
        
        const wxColour text_colour = isActive ? activeText : inactiveText;
        
        // Draw tab background for active tab or hovered tab except first tab is active
        dc.SetBrush(wxBrush(tab_colour));
        dc.SetPen(wxPen(tab_colour, 0)); 

        if ((!isFirstTab && isActive) || (mouseOverTab && !isActive)) {
            dc.DrawRectangle(tab_rect);
        }
        
        // Draw icon and text
        drawTabContent(dc, wnd, tab_rect, icon, text, text_colour, isFirstTab);

        // Draw close button for non-first tabs when mouse is over the tab or tab is active
        if (!isFirstTab && (isActive || mouseOverTab)) {
            drawCloseButton(dc, wnd, tab_rect, close_button_state, out_button_rect);
        }
        
        // Draw separator
        DrawTabSeparator(dc, wnd, tab_rect.x + tab_rect.width, tab_rect.y, tab_rect.height);
             
        // Set output parameters
        if (out_tab_rect) *out_tab_rect = tab_rect;
        if (x_extent) *x_extent = tabWidth;
    }
    
    void DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override
    {
        const TabMetrics metrics = getMetrics(wnd);
        wxColour activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator;
        getColorScheme(activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator);

        // Draw header background
        auto headerRect = rect;
        headerRect.y = rect.y + metrics.borderWidth;
        dc.SetPen(wxPen(tabHeaderBackground, 0));
        dc.SetBrush(wxBrush(tabHeaderBackground));
        dc.DrawRectangle(headerRect);

        // Draw header bottom border
        dc.SetPen(wxPen(border, metrics.borderWidth));
        dc.DrawLine(rect.x, rect.y + rect.height - metrics.borderWidth, rect.x + rect.width, rect.y + rect.height - metrics.borderWidth);
    }

    int GetBorderWidth(wxWindow* wnd) override {
        return 0; // Reserve space for top and bottom borders
    }

    wxSize GetTabSize(wxReadOnlyDC& dc, wxWindow* wnd, const wxString& caption, const wxBitmapBundle& bitmap, bool active, int close_button_state, int* x_extent) override {
        const TabMetrics metrics = getMetrics(wnd);
        // Get the default tab size
        wxAuiSimpleTabArt::GetTabSize(dc, wnd, caption, bitmap, active, close_button_state, x_extent);
        const int tab_width = calculateTabWidth(wnd, caption == FIRST_TAB_NAME);
        if (x_extent) {
            *x_extent = tab_width;
        }
        // Return custom size with modified height
        return wxSize(tab_width, metrics.height);
    }
    void DrawBorder(wxDC& dc, wxWindow* wnd, const wxRect& rect) override
    {
        const TabMetrics metrics = getMetrics(wnd);
        wxColour activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator;
        getColorScheme(activeTab, inactiveTab, hoverTab, activeText, inactiveText, background, border, tabHeaderBackground, separator);

        // Draw the main background
        dc.SetBrush(wxBrush(background));
        dc.SetPen(wxPen(background, 0));
        dc.DrawRectangle(rect);

        auto headerRect = rect;
        headerRect.height = metrics.height + metrics.borderWidth;
        if(headerRect.height <= rect.height) {
            dc.SetPen(wxPen(tabHeaderBackground, 0));
            dc.SetBrush(wxBrush(tabHeaderBackground));
            dc.DrawRectangle(headerRect);
        }
        // Draw header top border
        dc.SetPen(wxPen(border, metrics.borderWidth));
        dc.DrawLine(headerRect.x, headerRect.y, headerRect.x + headerRect.width, headerRect.y);
    }

    // Draw separator between tabs
    void DrawTabSeparator(wxDC& dc, wxWindow* wnd, int x, int y, int height) const
    {
        const TabMetrics metrics = getMetrics(wnd);
        wxColour separator_color = isDarkMode() ? wxColour(80, 80, 80) : wxColour(120, 120, 120);
        dc.SetPen(wxPen(separator_color, metrics.separatorWidth));
        dc.DrawLine(x, y, x, y + height);
    }
};


PrinterManagerView::PrinterManagerView(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL);
    
    mTabBar = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ALL_TABS | wxBORDER_NONE);
    mTabBar->SetArtProvider(new TabArt());
    mTabBar->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    mainSizer->Add(mTabBar, 1, wxEXPAND);
    SetSizer(mainSizer);
    wxAuiManager *m = (wxAuiManager*)&mTabBar->GetAuiManager();
    m->GetArtProvider()->SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, 0);
    // Delay creating and loading WebView until window is shown
    // This fixes macOS multi-display rendering issue where WKWebView fails to render
    // on extended displays if loaded before the window is fully displayed
    // The WebView will be created and loaded by calling initializeWebView() after window is shown
    mBrowser = nullptr;
    mResetWebViewOnShow = wxGetApp().is_recreating_gui();
    // Note: loadTabState() will be called in initializeWebView() after WebView is created

    Bind(wxEVT_CLOSE_WINDOW, &PrinterManagerView::onClose, this);
    mTabBar->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &PrinterManagerView::onClosePrinterTab, this);
    mTabBar->Bind(wxEVT_AUINOTEBOOK_BEGIN_DRAG, &PrinterManagerView::onTabBeginDrag, this);
    mTabBar->Bind(wxEVT_AUINOTEBOOK_DRAG_MOTION, &PrinterManagerView::onTabDragMotion, this);
    mTabBar->Bind(wxEVT_AUINOTEBOOK_END_DRAG, &PrinterManagerView::onTabEndDrag, this);
    mTabBar->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &PrinterManagerView::onTabChanged, this);
    BOOST_LOG_TRIVIAL(info) << "PrinterManagerView: constructed (WebView deferred)";
}

void PrinterManagerView::msw_rescale()
{
    if (!mTabBar) {
        return;
    }

    mTabBar->SetArtProvider(new TabArt());
    mTabBar->InvalidateBestSize();
    mTabBar->Layout();
    mTabBar->Refresh();
    mTabBar->Update();

    Layout();
    Refresh();
}

bool PrinterManagerView::createMainBrowser()
{
    mBrowser = WebView::CreateWebView(mTabBar, "");
    if (mBrowser == nullptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": could not init mBrowser";
        return false;
    }

    mIpc = std::make_unique<webviewIpc::WebviewIPCManager>(mBrowser);
    setupIPCHandlers();

    if (mTabBar->GetPageIndex(mBrowser) == wxNOT_FOUND) {
        if (mTabBar->GetPageCount() == 0)
            mTabBar->AddPage(mBrowser, FIRST_TAB_NAME);
        else
            mTabBar->InsertPage(0, mBrowser, FIRST_TAB_NAME);
    }

    return true;
}

void PrinterManagerView::resetMainBrowser()
{
    mIpc.reset();

    if (mBrowser) {
        const int pageIndex = mTabBar ? mTabBar->GetPageIndex(mBrowser) : wxNOT_FOUND;
        if (mTabBar && pageIndex != wxNOT_FOUND)
            mTabBar->RemovePage(pageIndex);
        mBrowser->Destroy();
        mBrowser = nullptr;
    }

    mIsReady = false;
    mWebViewInitialized = false;
}

bool PrinterManagerView::Show(bool show)
{
    bool result = wxPanel::Show(show);
    if (show && mResetWebViewOnShow) {
        mResetWebViewOnShow = false;
        resetMainBrowser();
        initializeWebView();
    }
    return result;
}

void PrinterManagerView::initializeWebView()
{
    // Only initialize once
    bool expected = false;
    if (!mWebViewInitialized.compare_exchange_strong(expected, true)) {
        return;
    }
    
    // Create WebView if not already created
    if (!mBrowser) {
        if (!createMainBrowser()) {
            mWebViewInitialized = false;
            return;
        }
    }
    
    // Configure and load URL
    mBrowser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    
    // Load URL
    auto _dir = resources_dir();
    std::replace(_dir.begin(), _dir.end(), '\\', '/');
    wxString TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/printer/printer_manager/printer.html").make_preferred().string());
    TargetUrl = "file://" + TargetUrl;
    wxString strlang = wxGetApp().current_language_code_safe();
    if (strlang != "")
        TargetUrl = wxString::Format("%s?lang=%s", TargetUrl, strlang);
    if(wxGetApp().app_config->get_bool("developer_mode")){
        TargetUrl = TargetUrl + "&dev=true";
    }  
    WebView::LoadUrl(mBrowser, TargetUrl);
    
    // Set ElegooSlicer UserAgent
    wxString theme = wxGetApp().dark_mode() ? "dark" : "light";
#ifdef __WIN32__
    mBrowser->SetUserAgent(wxString::Format("ElegooSlicer/%s (%s) Mozilla/5.0 (Windows NT 10.0; Win64; x64)", 
        ELEGOOSLICER_VERSION, theme));
#elif defined(__WXMAC__)
    mBrowser->SetUserAgent(wxString::Format("ElegooSlicer/%s (%s) Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)", 
        ELEGOOSLICER_VERSION, theme));
#elif defined(__linux__)
    mBrowser->SetUserAgent(wxString::Format("ElegooSlicer/%s (%s) Mozilla/5.0 (X11; Linux x86_64)", 
        ELEGOOSLICER_VERSION, theme));
#else
    mBrowser->SetUserAgent(wxString::Format("ElegooSlicer/%s (%s) Mozilla/5.0 (compatible; ElegooSlicer)", 
        ELEGOOSLICER_VERSION, theme));
#endif

    // Load saved tab state
    loadTabState();
    
    Layout();
    BOOST_LOG_TRIVIAL(info) << "PrinterManagerView::initializeWebView: complete";
}

PrinterManagerView::~PrinterManagerView() {
    BOOST_LOG_TRIVIAL(info) << "PrinterManagerView: destructor";
    // Save tab state before destruction
    saveTabState();

    if (mConnectStatusChangedHandlerId != 0) {
        PrinterNetworkEvent::getInstance()->connectStatusChanged.disconnect(mConnectStatusChangedHandlerId);
        mConnectStatusChangedHandlerId = 0;
    }
    if (mEventRawChangedHandlerId != 0) {
        PrinterNetworkEvent::getInstance()->eventRawChanged.disconnect(mEventRawChangedHandlerId);
        mEventRawChangedHandlerId = 0;
    }
    if (mRtcTokenChangedHandlerId != 0) {
        UserNetworkEvent::getInstance()->rtcTokenChanged.disconnect(mRtcTokenChangedHandlerId);
        mRtcTokenChangedHandlerId = 0;
    }
    if (mRtmMessageChangedHandlerId != 0) {
        UserNetworkEvent::getInstance()->rtmMessageChanged.disconnect(mRtmMessageChangedHandlerId);
        mRtmMessageChangedHandlerId = 0;
    }

    std::lock_guard<std::mutex> lock(mPrinterViewsMutex);
    mPrinterViews.clear();
}

void PrinterManagerView::openPrinterTab(const std::string& printerId, bool saveState, bool openDeviceAssistant,
                                        bool focusTab)
{
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();

    ElegooPrinterWebView* existingView = findPrinterView(printerId);
    if (existingView) {
        int idx = mTabBar->GetPageIndex(existingView);
        if (idx != wxNOT_FOUND) {
            if (focusTab) {
                mTabBar->SetSelection(idx);
            }
            if (openDeviceAssistant) {
                nlohmann::json data;
                data["printerId"] = printerId;
                data["openDeviceAssistant"] = true;
                data["timestamp"] = nowMs;
                existingView->onOpenDeviceAssistant(data);
            }
            Layout();
            return;
        }
    }
    std::vector<PrinterNetworkInfo> printerList = PrinterManager::getInstance()->getPrinterList();
    PrinterNetworkInfo printerInfo;
    for (auto& printer : printerList) {
        if (printer.printerId == printerId) {
            printerInfo = printer;
            break;  
        }
    }
    if (printerInfo.printerId.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": printer %s not found") % printerId;
        return;
    }

    wxString url = from_u8(printerInfo.webUrl);
    if(url.IsEmpty()) {
        return;
    }

    auto appendUrlParam = [&url](const std::string& key, const std::string& value) {
        if (url.Contains("?")) {
            url += "&" + key + "=" + from_u8(value);
        } else {
            url += "?" + key + "=" + from_u8(value);
        }
    };

    ElegooPrinterWebView* view = new ElegooPrinterWebView(mTabBar);

    if(PrintHost::get_print_host_type(printerInfo.hostType) == htElegooLink && (printerInfo.printerModel == "Elegoo Centauri Carbon 2" || printerInfo.printerModel == "Elegoo Centauri 2")) 
    {
        std::string accessCode = printerInfo.accessCode;
        url = url + wxString("?id=") + from_u8(printerInfo.printerId) + "&ip=" + printerInfo.host +"&sn=" + from_u8(printerInfo.serialNumber) + "&access_code=" + accessCode;
    }

    // Get the login region option and add it to the URL parameters
    std::string region = wxGetApp().app_config->get("region");
    if (!region.empty()) {
        // Convert region value to URL encoded format
        std::string region_encoded = wxGetApp().url_encode(region);

        appendUrlParam("region", region_encoded);
    }

    // Add current language parameter
    wxString lang = wxGetApp().current_language_code_safe();
    if (!lang.empty()) {
        appendUrlParam("lang", lang.ToStdString());
    }

    if(wxGetApp().app_config->get_bool("developer_mode")){
        appendUrlParam("dev", "true");
    }

    if (openDeviceAssistant) {
        appendUrlParam("openDeviceAssistant", "true");
        appendUrlParam("timestamp", std::to_string(nowMs));
    }

    view->setPrinterModel(printerInfo.printerModel);
    view->load_url(url);
    // Local network shows IP address, cloud printing shows printer name
    if(printerInfo.networkType==0)
    {
        mTabBar->AddPage(view, from_u8(printerInfo.host));
    }else {
        mTabBar->AddPage(view, from_u8(printerInfo.printerName));
    }
    if (focusTab) {
        mTabBar->SetSelection(mTabBar->GetPageCount() - 1);
    }
    insertPrinterView(printerId, view);
    Layout();
    
    // Update tab state after adding new tab
    if(saveState)
        saveTabState();
}

void PrinterManagerView::refreshUserInfo()
{
    lock_guard<mutex> lock(mUserInfoMutex);
    UserNetworkInfo userNetworkInfo = UserNetworkManager::getInstance()->getUserInfo();
    if (mIpc && mIsReady) {
    // Send refresh signal to navigation webview via IPC
        nlohmann::json data = convertUserNetworkInfoToJson(userNetworkInfo);
        mRefreshUserInfo = UserNetworkInfo();
        mIpc->sendEvent("onUserInfoUpdated", data, mIpc->generateRequestId());
    } else {
        mRefreshUserInfo = userNetworkInfo;
    }
}

void PrinterManagerView::onClose(wxCloseEvent& evt)
{
    // this->Hide();
}

void PrinterManagerView::onClosePrinterTab(wxAuiNotebookEvent& event)
{
    int page = event.GetSelection();
    if (page == wxNOT_FOUND) return;

    wxWindow* win = mTabBar->GetPage(page);
    ElegooPrinterWebView* viewToClose = removePrinterViewByWindow(win);
    if (viewToClose) {
        viewToClose->OnClose(wxCloseEvent());
        mTabBar->SetSelection(0);
    }
    
    // Update tab state after closing tab
    saveTabState();
    
    event.Skip();
}

void PrinterManagerView::onTabBeginDrag(wxAuiNotebookEvent& event)
{
    // Prevent dragging the first tab (index 0)
    if ( event.GetSelection() == 0  ) {
        event.Veto();
        mFirstTabClicked = true;
        return;
    }
    mFirstTabClicked = false;
    event.Skip();
}

void PrinterManagerView::onTabDragMotion(wxAuiNotebookEvent& event)
{
    if (mFirstTabClicked) {
        event.Veto();
        return;
    }
    // Prevent dropping at position 0
    wxPoint screenPt = wxGetMousePosition();
    wxPoint nbPt = mTabBar->ScreenToClient(screenPt);
    long flags = 0;
    int targetIndex = mTabBar->HitTest(nbPt, &flags);   
    if (targetIndex == 0) {
        event.Veto();
        return;
    }
    event.Skip();
}

void PrinterManagerView::onTabEndDrag(wxAuiNotebookEvent& event)
{
    if (mFirstTabClicked) {
        event.Veto();
        return;
    }   
    saveTabState();
    event.Skip();
}

void PrinterManagerView::onTabChanged(wxAuiNotebookEvent& event)
{
    // Save tab state when active tab changes
    saveTabState();
    event.Skip();
}



void PrinterManagerView::setupIPCHandlers()
{
    if (!mIpc) return;

    // Handle request_printer_list
    mIpc->onRequest("request_printer_list", [this](const IPCRequest& request){
        return getPrinterList();
    });

    // Handle request_printer_model_list
    mIpc->onRequest("request_printer_model_list", [this](const IPCRequest& request){
        return getPrinterModelList();
    });


    // Handle request_printer_detail
    mIpc->onRequest("request_printer_detail", [this](const IPCRequest& request){
        auto params = request.params;
        std::string printerId = params.value("printerId", "");
        bool openDeviceAssistant = params.value("openDeviceAssistant", false);
        if (!printerId.empty()) {
            wxGetApp().CallAfter([this, printerId, openDeviceAssistant]() {
                openPrinterTab(printerId, true, openDeviceAssistant);
            });
        }
        return IPCResult::success();
    });

    // Handle request_discover_printers (async due to time-consuming operation)
    mIpc->onRequestAsync("request_discover_printers", [this](const IPCRequest& request, 
                                                           std::function<void(const IPCResult&)> sendResponse) {  
        try {
            // Call the static method to avoid accessing this pointer
            auto result = this->discoverPrinter();
            sendResponse(result);
        } catch (const std::exception& e) {
            sendResponse(IPCResult::error(std::string("Discovery failed: ") + e.what()));
        }
    });

    // Handle request_add_printer (async)
    mIpc->onRequestAsync("request_add_printer", [this](const IPCRequest& request,
                                                        std::function<void(const IPCResult&)> sendResponse) {  
        auto params = request.params;
        if (!params.contains("printer")) {
            sendResponse(IPCResult::error("Missing printer parameter"));
            return;
        }
        nlohmann::json printer = params["printer"];
        
        try {
            auto result = addPrinter(printer);
            sendResponse(result);        
        } catch (const std::exception& e) {
            sendResponse(IPCResult::error(std::string("Add printer failed: ") + e.what()));
        } catch (...) {
            sendResponse(IPCResult::error("Add printer failed: Unknown error"));
        }
    });

    // Handle request_cancel_add_printer
    mIpc->onRequestAsync("request_cancel_add_printer", [this](const IPCRequest& request,
                                                                 std::function<void(const IPCResult&)> sendResponse) {
        auto params = request.params;
        if (!params.contains("printer")) {
            sendResponse(IPCResult::error("Missing printer parameter"));
            return;
        }
        nlohmann::json printer = params["printer"];
        try {
            auto result = cancelBindPrinter(printer);
            sendResponse(result);
        } catch (const std::exception& e) {
            sendResponse(IPCResult::error(std::string("Cancel bind printer failed: ") + e.what()));
        } catch (...) {
            sendResponse(IPCResult::error("Cancel bind printer failed: Unknown error"));
        }
    });
    // Handle request_add_physical_printer (async)
    mIpc->onRequestAsync("request_add_physical_printer", [this](const IPCRequest& request,
                                                                 std::function<void(const IPCResult&)> sendResponse) {
      
        auto params = request.params;
        if (!params.contains("printer")) {
            sendResponse(IPCResult::error("Missing printer parameter"));
            return;
        }
        
        nlohmann::json printer = params["printer"];
            try {
                auto result = addPhysicalPrinter(printer);   
                sendResponse(result);
            } catch (const std::exception& e) {
                sendResponse(IPCResult::error(std::string("Add physical printer failed: ") + e.what()));
            } catch (...) {
                sendResponse(IPCResult::error("Add physical printer failed: Unknown error"));
            }
    });

    // Handle request_update_printer_name
    mIpc->onRequest("request_update_printer_name", [this](const IPCRequest& request){
        auto params = request.params;
        std::string printerId = params.value("printerId", "");
        std::string printerName = params.value("printerName", "");
        return updatePrinterName(printerId, printerName);
    });

    // Handle request_update_physical_printer
    mIpc->onRequestAsync("request_update_physical_printer", [this](const IPCRequest& request,
                                                                    std::function<void(const IPCResult&)> sendResponse) {
        auto params = request.params;
        if (!params.contains("printerId") || !params.contains("printer")) {
            sendResponse(IPCResult::error("missing printerId or printer parameter"));
            return;
        }
        
        std::string printerId = params.value("printerId", "");
        nlohmann::json printer = params["printer"];

        try {
            auto result = updatePhysicalPrinter(printerId, printer);
            sendResponse(result);
        } catch (const std::exception& e) {
            sendResponse(IPCResult::error(std::string("update physical printer failed: ") + e.what()));
        } catch (...) {
            sendResponse(IPCResult::error("update physical printer failed: unknown error"));
        }
    });

    // Handle request_update_printer_host
    mIpc->onRequestAsync("request_update_printer_host", [this](const IPCRequest& request,
                                                                std::function<void(const IPCResult&)> sendResponse) {
        auto params = request.params;
        std::string printerId = params.value("printerId", "");
        std::string host = params.value("host", "");
 
            try {
                auto result = updatePrinterHost(printerId, host);
                sendResponse(result);
            } catch (const std::exception& e) {
                sendResponse(IPCResult::error(std::string("Update printer host failed: ") + e.what()));
            } catch (...) {
                sendResponse(IPCResult::error("Update printer host failed: Unknown error"));    
            }
    });

    // Handle request_delete_printer
    mIpc->onRequest("request_delete_printer", [this](const IPCRequest& request){
        auto params = request.params;
        std::string printerId = params.value("printerId", "");
        return deletePrinter(printerId);
    });

    // Handle request_browse_ca_file (async because it shows a file dialog)
    mIpc->onRequestAsync("request_browse_ca_file", [this](const IPCRequest& request,
                                                          std::function<void(const IPCResult&)> sendResponse) {
        wxGetApp().CallAfter([this, sendResponse]() {
            try {
                IPCResult result = browseCAFile();
                sendResponse(result);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": error in browseCAFile: %s") % e.what();
                sendResponse(IPCResult::error(std::string("Failed to browse CA file: ") + e.what()));
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unknown error in browseCAFile";
                sendResponse(IPCResult::error("Failed to browse CA file: Unknown error"));
            }
        });
    });

    // Handle request_refresh_printers
    mIpc->onRequest("request_refresh_printers", [this](const IPCRequest& request){
        // Implementation for refresh printers
        return IPCResult::success();
    });

    // Handle request_refresh_wan_printers
    mIpc->onRequest("request_refresh_wan_printers", [this](const IPCRequest& request){
        PrinterManager::getInstance()->enqueueWanSyncRequest();
        return IPCResult::success();
    });
    // Handle request_logout_print_host
    mIpc->onRequest("request_logout_print_host", [this](const IPCRequest& request){
        // Implementation for logout print host
        return IPCResult::success();
    });

    // Handle request_connect_physical_printer
    mIpc->onRequest("request_connect_physical_printer", [this](const IPCRequest& request){
        // Implementation for connect physical printer
        return IPCResult::success();
    });

    mIpc->onRequest("checkLoginStatus", [this](const IPCRequest& request){
        return handleCheckLoginStatus();
    });

    mIpc->onRequest("ready", [this](const IPCRequest& request){
        return handleReady();
    });

    mIpc->onRequest("request_user_info", [this](const IPCRequest& request){
        UserNetworkInfo userNetworkInfo = UserNetworkManager::getInstance()->getUserInfo();   
        nlohmann::json data = convertUserNetworkInfoToJson(userNetworkInfo);
        return IPCResult::success(data);
    });

    mIpc->onRequest("get_license_expired_devices", [this](const IPCRequest& request){
        auto licenseResult = PrinterManager::getInstance()->getLicenseExpiredDevices();
        IPCResult result;
        nlohmann::json devicesJson;
        devicesJson["devices"] = nlohmann::json::array();
        if (licenseResult.hasData()) {
            for (const auto& device : licenseResult.data.value()) {
                nlohmann::json deviceJson;
                deviceJson["serialNumber"] = device.serialNumber;
                deviceJson["status"] = device.status;
                devicesJson["devices"].push_back(deviceJson);
            }
        }
        result.data = devicesJson;
        result.message = licenseResult.message;
        result.code = licenseResult.isSuccess() ? 0 : static_cast<int>(licenseResult.code);
        return result;
    });
    
    mIpc->onRequest("renew_license", [this](const IPCRequest& request){
        auto params = request.params;
        std::string serialNumber = params.value("serialNumber", "");
        auto renewResult = PrinterManager::getInstance()->renewLicense(serialNumber);
        IPCResult result;
        result.message = renewResult.message;
        result.code = renewResult.isSuccess() ? 0 : static_cast<int>(renewResult.code);
        return result;
    });

    mIpc->onRequest("get_license_expired_devices", [this](const IPCRequest& request){
        auto licenseResult = PrinterManager::getInstance()->getLicenseExpiredDevices();
        IPCResult result;
        nlohmann::json devicesJson;
        devicesJson["devices"] = nlohmann::json::array();
        if (licenseResult.hasData()) {
            for (const auto& device : licenseResult.data.value()) {
                nlohmann::json deviceJson;
                deviceJson["serialNumber"] = device.serialNumber;
                deviceJson["status"] = device.status;
                devicesJson["devices"].push_back(deviceJson);
            }
        }
        result.data = devicesJson;
        result.message = licenseResult.message;
        result.code = licenseResult.isSuccess() ? 0 : static_cast<int>(licenseResult.code);
        return result;
    });
    
    mIpc->onRequest("renew_license", [this](const IPCRequest& request){
        auto params = request.params;
        std::string serialNumber = params.value("serialNumber", "");
        auto renewResult = PrinterManager::getInstance()->renewLicense(serialNumber);
        IPCResult result;
        result.message = renewResult.message;
        result.code = renewResult.isSuccess() ? 0 : static_cast<int>(renewResult.code);
        return result;
    });
    
    mIpc->onRequest("refresh_printer_status", [this](const IPCRequest& request){
        IPCResult result;
        auto printerList = PrinterManager::getInstance()->getPrinterList(); 
        for (const auto& printer : printerList) {
            auto refreshResult = PrinterManager::getInstance()->refreshPrinterStatus(printer.printerId);
        }
        result.code = 0;
        return result;
    });
    
    mConnectStatusChangedHandlerId = PrinterNetworkEvent::getInstance()->connectStatusChanged.connect([this](const PrinterConnectStatusEvent& event) {
        wxGetApp().CallAfter([this, event] {
            ElegooPrinterWebView* targetView = findPrinterView(event.printerId);
            nlohmann::json data;
            data["status"] = event.status;
            data["printerId"] = event.printerId;
            if (targetView) {
                targetView->onConnectionStatus(data);
            }
        });
    });
    mEventRawChangedHandlerId = PrinterNetworkEvent::getInstance()->eventRawChanged.connect([this](const PrinterEventRawEvent& event) {
        wxGetApp().CallAfter([this, event] {
            ElegooPrinterWebView* targetView = findPrinterView(event.printerId);
            nlohmann::json data;
            data["event"] = event.event;
            data["printerId"] = event.printerId;
            if (targetView) {
                targetView->onPrinterEventRaw(data);
            }
        });
    });

    mRtcTokenChangedHandlerId = UserNetworkEvent::getInstance()->rtcTokenChanged.connect([this](const UserRtcTokenEvent& event) {
        wxGetApp().CallAfter([this, event] {
            nlohmann::json data;
            data["rtcToken"] = event.userInfo.rtcToken;
            data["userId"] = event.userInfo.userId;
            data["rtcTokenExpireTime"] = event.userInfo.rtcTokenExpireTime;
            forEachPrinterView([&data](const std::string&, ElegooPrinterWebView* view) { view->onRtcTokenChanged(data); });
        });
    });
    mRtmMessageChangedHandlerId = UserNetworkEvent::getInstance()->rtmMessageChanged.connect([this](const UserRtmMessageEvent& event) {
        wxGetApp().CallAfter([this, event] {
            ElegooPrinterWebView* targetView = findPrinterView(event.printerId);
            nlohmann::json data;
            data["message"] = event.message;
            data["printerId"] = event.printerId;
            if (targetView) {
                targetView->onRtmMessage(data);
            }
        });
    });

}

IPCResult PrinterManagerView::deletePrinter(const std::string& printerId)
{ 
    IPCResult result;
    auto networkResult = PrinterManager::getInstance()->deletePrinter(printerId);
    result.message = networkResult.message;
    result.code = networkResult.isSuccess() ? 0 : static_cast<int>(networkResult.code);
    wxGetApp().CallAfter([this, printerId]() {
        ElegooPrinterWebView* view = findPrinterView(printerId);
        if (view) {
            int page = mTabBar->GetPageIndex(view);
            if (page != wxNOT_FOUND) {
                mTabBar->DeletePage(page);
            }
            view->OnClose(wxCloseEvent());
            removePrinterView(printerId);
            mTabBar->SetSelection(0);
        }
    });
    return result;
}
void PrinterManagerView::closeInvalidPrinterTab(std::vector<PrinterNetworkInfo>& printerList)
{
    std::vector<std::string> printersToRemove;
    std::vector<ElegooPrinterWebView*> viewsToClose;
    
    forEachPrinterView([&printerList, &printersToRemove, &viewsToClose](const std::string& printerId, ElegooPrinterWebView* view) {
        auto it = std::find_if(printerList.begin(), printerList.end(), 
                              [&printerId](const PrinterNetworkInfo& p) { return p.printerId == printerId; });
        if (it == printerList.end()) {
            printersToRemove.push_back(printerId);
            viewsToClose.push_back(view);
        }
    });
    
    wxGetApp().CallAfter([this, printersToRemove, viewsToClose]() {
        for (size_t i = 0; i < printersToRemove.size(); ++i) {
            int page = mTabBar->GetPageIndex(viewsToClose[i]);
            if (page != wxNOT_FOUND) {
                mTabBar->DeletePage(page);
            }
            viewsToClose[i]->OnClose(wxCloseEvent());
            removePrinterView(printersToRemove[i]);
        }
    });

}
IPCResult PrinterManagerView::updatePrinterName(const std::string& printerId, const std::string& printerName)
{
    IPCResult result;
    wxGetApp().CallAfter([this, printerId, printerName]() {
        ElegooPrinterWebView* view = findPrinterView(printerId);
        if (view) {
            int page = mTabBar->GetPageIndex(view);
            if (page != wxNOT_FOUND) {
                mTabBar->SetPageText(page, from_u8(printerName));
            }
        }
    });
    auto networkResult = PrinterManager::getInstance()->updatePrinterName(printerId, printerName);
    result.message = networkResult.message;
    result.code = networkResult.isSuccess() ? 0 : static_cast<int>(networkResult.code);
    return result;
}
IPCResult PrinterManagerView::updatePrinterHost(const std::string& printerId, const std::string& host)
{
    IPCResult result;
    auto networkResult = PrinterManager::getInstance()->updatePrinterHost(printerId, host);
    result.message = networkResult.message;
    result.code = networkResult.isSuccess() ? 0 : static_cast<int>(networkResult.code);
    PrinterNetworkInfo printerInfo = PrinterManager::getInstance()->getPrinterNetworkInfo(printerId);
    
    if(result.code == 0 && !printerInfo.host.empty() && printerInfo.host != host) {     
        wxString url = printerInfo.webUrl;
        if(PrintHost::get_print_host_type(printerInfo.hostType) == htElegooLink && (printerInfo.printerModel == "Elegoo Centauri Carbon 2" || printerInfo.printerModel == "Elegoo Centauri 2")) 
        {
            std::string accessCode = printerInfo.accessCode;
            url = url + wxString("?id=") + from_u8(printerInfo.printerId) + "&ip=" + printerInfo.host +"&sn=" + from_u8(printerInfo.serialNumber) + "&access_code=" + accessCode;
        }
        wxGetApp().CallAfter([this, printerId, url]() {
            ElegooPrinterWebView* view = findPrinterView(printerId);
            if (view) {
                int page = mTabBar->GetPageIndex(view);
                if (page != wxNOT_FOUND) {
                    view->load_url(url);
                }
            }
        });
    }
    return result;
}

IPCResult PrinterManagerView::updatePhysicalPrinter(const std::string& printerId, const nlohmann::json& printer)
{
    IPCResult result;
    PrinterNetworkInfo printerInfo = convertJsonToPrinterNetworkInfo(printer);

    PrinterNetworkInfo oldPrinter =  PrinterManager::getInstance()->getPrinterNetworkInfo(printerId);
    if (oldPrinter.printerId.empty()) {
        result.code = static_cast<int>(PrinterNetworkErrorCode::PRINTER_NOT_FOUND);
        result.message = getErrorMessage(PrinterNetworkErrorCode::PRINTER_NOT_FOUND);
        return result;
    }

    auto networkResult = PrinterManager::getInstance()->updatePhysicalPrinter(printerId, printerInfo);
    result.message = networkResult.message;
    result.code = networkResult.isSuccess() ? 0 : static_cast<int>(networkResult.code);
    
    if (result.code == 0 && (oldPrinter.host != printerInfo.host || oldPrinter.webUrl != printerInfo.webUrl)) {
        PrinterNetworkInfo updatedPrinter = PrinterManager::getInstance()->getPrinterNetworkInfo(printerId);
        wxGetApp().CallAfter([this, printerId, updatedPrinter]() {
            ElegooPrinterWebView* view = findPrinterView(printerId);
            if (view) {
                int page = mTabBar->GetPageIndex(view);
                if (page != wxNOT_FOUND) {
                    mTabBar->SetPageText(page, from_u8(updatedPrinter.printerName));
                    wxString url = updatedPrinter.webUrl;
                    view->load_url(url);
                    view->reload();
                }
            }
        });
    }
    return result;
}

IPCResult PrinterManagerView::addPrinter(const nlohmann::json& printer)
{
    IPCResult result;
    PrinterNetworkInfo printerInfo = convertJsonToPrinterNetworkInfo(printer);
    printerInfo.isPhysicalPrinter = false;
    TelemetryTimer connect_timer;
    auto networkResult = PrinterManager::getInstance()->addPrinter(printerInfo);
    result.message = networkResult.message;
    result.code = networkResult.isSuccess() ? 0 : static_cast<int>(networkResult.code);

    TelemetryEvents::report_printer_manual_connect(printerInfo, result.code, connect_timer.elapsed_ms());

    return result;
}
IPCResult PrinterManagerView::addPhysicalPrinter(const nlohmann::json& printer)
{
    IPCResult result;
    PrinterNetworkErrorCode errorCode = PrinterNetworkErrorCode::SUCCESS;
    PrinterNetworkInfo printerInfo;
    TelemetryTimer connect_timer;
    try {
        printerInfo = convertJsonToPrinterNetworkInfo(printer);
        printerInfo.isPhysicalPrinter = true;
        auto networkResult = PrinterManager::getInstance()->addPrinter(printerInfo);
        result.message = networkResult.message;
        errorCode = networkResult.code;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": add physical printer error: %s") % e.what();
        errorCode = PrinterNetworkErrorCode::INVALID_PARAMETER;
        result.message = getErrorMessage(errorCode);
    }
    result.code = errorCode == PrinterNetworkErrorCode::SUCCESS ? 0 : static_cast<int>(errorCode);

    TelemetryEvents::report_printer_manual_connect(printerInfo, result.code, connect_timer.elapsed_ms());

    return result;
}

IPCResult PrinterManagerView::cancelBindPrinter(const nlohmann::json& printer)
{
    IPCResult result;
    PrinterNetworkInfo printerInfo = convertJsonToPrinterNetworkInfo(printer);
    auto networkResult = PrinterManager::getInstance()->cancelBindPrinter(printerInfo);
    result.message = networkResult.message;
    result.code = networkResult.isSuccess() ? 0 : static_cast<int>(networkResult.code);
    return result;
}
IPCResult PrinterManagerView::discoverPrinter()
{
    IPCResult result;
    auto printerListResult = PrinterManager::getInstance()->discoverPrinter();
    std::vector<PrinterNetworkInfo> printerList;
    if(printerListResult.hasData()) {
        printerList = printerListResult.data.value();
    }
    nlohmann::json response = json::array();
    for (auto& printer : printerList) {
        nlohmann::json printer_obj = nlohmann::json::object();
        printer_obj = convertPrinterNetworkInfoToJson(printer);
        printer_obj["isAdded"] = printer.isAdded;
        boost::filesystem::path resources_path(Slic3r::resources_dir());
        std::string img_path = resources_path.string() + "/profiles/" + printer.vendor + "/" + printer.printerModel + "_cover.png";
        printer_obj["printerImg"] = PrinterManager::imageFileToBase64DataURI(img_path);
        response.push_back(printer_obj);
    }
    result.data = response;
    result.code = printerListResult.isSuccess() ? 0 : static_cast<int>(printerListResult.code);
    result.message = printerListResult.message;
    return result;
}
IPCResult PrinterManagerView::getPrinterList()
{  
    IPCResult result;
    // Cache for printer images (printerId -> base64 image data)
    static std::map<std::string, std::string> printerImageCache;
    auto printerList = PrinterManager::getInstance()->getPrinterList();
    bool shouldReportSnapshot = false;

    {
        std::lock_guard<std::mutex> lock(mPrinterSnapshotMutex);
        const std::size_t currentPrinterCount = printerList.size();
        if (!mHasObservedPrinterCount || mLastObservedPrinterCount != currentPrinterCount) {
            mLastObservedPrinterCount = currentPrinterCount;
            mHasObservedPrinterCount = true;
            shouldReportSnapshot = true;
        }
    }
    
    // Build set of current printer IDs and process printers in one pass
    std::set<std::string> currentPrinterIds;
    nlohmann::json response = json::array();
    boost::filesystem::path resources_path(Slic3r::resources_dir());
    
    for (auto& printer : printerList) {
        currentPrinterIds.insert(printer.printerId);
        
        nlohmann::json printer_obj = convertPrinterNetworkInfoToJson(printer);
        
        // Check if image is already cached
        auto cacheIt = printerImageCache.find(printer.printerId);
        if (cacheIt == printerImageCache.end()) {
            // Load image and cache it
            std::string img_path = resources_path.string() + "/profiles/" + printer.vendor + "/" + printer.printerModel + "_cover.png";
            printerImageCache[printer.printerId] = PrinterManager::imageFileToBase64DataURI(img_path);
            cacheIt = printerImageCache.find(printer.printerId);
        }
        printer_obj["printerImg"] = cacheIt->second;
        response.push_back(printer_obj);
    }
    
    // Remove cached images for printers that no longer exist
    for (auto it = printerImageCache.begin(); it != printerImageCache.end();) {
        if (currentPrinterIds.find(it->first) == currentPrinterIds.end()) {
            it = printerImageCache.erase(it);
        } else {
            ++it;
        }
    }
    
    closeInvalidPrinterTab(printerList);

    if (shouldReportSnapshot) {
        TelemetryEvents::report_printer_list_snapshot(printerList);
    }

    // Return object with printer list and main client status
    nlohmann::json resultData;
    resultData["printers"] = response;
    result.data = resultData;
    result.code = 0;
    result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
    return result;
}
IPCResult PrinterManagerView::browseCAFile()
{
    IPCResult result;
    PrinterNetworkErrorCode errorCode = PrinterNetworkErrorCode::SUCCESS;
    std::string path = "";
    try {
        static const auto filemasks = _L("Certificate files (*.crt, *.pem)|*.crt;*.pem|All files|*.*");
        wxFileDialog openFileDialog(this, _L("Open CA certificate file"), "", "", filemasks, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (openFileDialog.ShowModal() != wxID_CANCEL) {
            path = openFileDialog.GetPath().ToStdString();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": browse CA file error: %s") % e.what();
        errorCode = PrinterNetworkErrorCode::PRINTER_UNKNOWN_ERROR;
    }
    result.data = path;
    result.code = errorCode == PrinterNetworkErrorCode::SUCCESS ? 0 : static_cast<int>(errorCode);
    result.message = getErrorMessage(errorCode);
    return result;
}
IPCResult PrinterManagerView::handleReady()
{
    lock_guard<mutex> lock(mUserInfoMutex);
    mIsReady = true;
    // Send any pending user info with delay to ensure frontend is ready
    if(!mRefreshUserInfo.userId.empty() && mIpc) {
        nlohmann::json data = convertUserNetworkInfoToJson(mRefreshUserInfo);
        mIpc->sendEvent("onUserInfoUpdated", data, mIpc->generateRequestId());
        mRefreshUserInfo = UserNetworkInfo();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": sent pending user info to WebView";
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": ready";
    return IPCResult::success();
}
IPCResult PrinterManagerView::handleCheckLoginStatus()
{
    UserNetworkInfo userNetworkInfo = UserNetworkManager::getInstance()->getUserInfo();
    auto            result          = UserNetworkManager::getInstance()->checkUserNeedReLogin();
    if (result.isSuccess()) {
        bool needReLogin = result.data.value();
        if (needReLogin) {
            TelemetryEvents::report_login_click();
            // need re-login
            auto evt = new wxCommandEvent(EVT_USER_LOGIN);
            wxQueueEvent(wxGetApp().mainframe, evt);
            return IPCResult::success();
        }
    } else {
        return IPCResult::error(result.message);
    }
    // don't need to re-login, return user info
    nlohmann::json data = convertUserNetworkInfoToJson(userNetworkInfo);
    return IPCResult::success(data);
}
IPCResult PrinterManagerView::getPrinterModelList()
{
    IPCResult result;
    auto vendorPrinterModelConfigMap = PrinterManager::getVendorPrinterModelConfig();   
    nlohmann::json response = nlohmann::json::array();
    for (auto& vendor : vendorPrinterModelConfigMap) {
        nlohmann::json vendorObj = nlohmann::json::object();
        vendorObj["vendor"]      = vendor.first;
        vendorObj["models"]      = nlohmann::json::array();
        for (auto& model : vendor.second) {
            nlohmann::json modelObj = nlohmann::json::object();
            modelObj["modelName"]  = model.first;
            auto config = model.second;
            const auto opt = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
            const auto hostType = opt != nullptr ? opt->value : htOctoPrint;
            std::string hostTypeStr = PrintHost::get_print_host_type_str(hostType);
            if (!hostTypeStr.empty()) {
                modelObj["hostType"]   = hostTypeStr;
            }
            bool supportWanNetwork = false;
            
            if (config.has("support_wan_network")) {
                supportWanNetwork = config.opt_bool("support_wan_network");
            }
            modelObj["supportWanNetwork"] = supportWanNetwork;
            vendorObj["models"].push_back(modelObj);
        }
        response.push_back(vendorObj);
    }
    result.data = response;
    result.code = 0;
    result.message = getErrorMessage(PrinterNetworkErrorCode::SUCCESS);
    return result;
}

void PrinterManagerView::saveTabState()
{
    std::lock_guard<std::mutex> lock(s_tabStateMutex);
    try {
        nlohmann::json tabState = nlohmann::json::object();
        nlohmann::json tabs = nlohmann::json::array();
        
        // Save all printer tabs (skip the first tab which is always "Connected Printer")
        for (size_t i = 1; i < mTabBar->GetPageCount(); ++i) {
            wxWindow* page = mTabBar->GetPage(i);
            if (page) {
                // Find the printer ID for this page
                forEachPrinterView([&page, &tabs, this, i](const std::string& printerId, ElegooPrinterWebView* view) {
                    if (view == page) {
                        nlohmann::json tabInfo;
                        tabInfo["printerId"] = printerId;
                        tabInfo["tabName"] = mTabBar->GetPageText(i).ToStdString();
                        tabs.push_back(tabInfo);
                    }
                });
            }
        }
        
        // Save current active tab index
        int activeTabIndex = mTabBar->GetSelection();
        tabState["tabs"] = tabs;
        tabState["activeTabIndex"] = activeTabIndex;
        
        // Save to file
        std::string filePath = (boost::filesystem::path(Slic3r::data_dir()) / "user" / "printer_tab_state.json").string();
        
        // Ensure directory exists
        boost::filesystem::path dir = boost::filesystem::path(filePath).parent_path();
        if (!boost::filesystem::exists(dir)) {
            boost::filesystem::create_directories(dir);
        }
        
        boost::nowide::ofstream file(filePath);
        if (file.is_open()) {
            file << tabState.dump(4);
            file.close();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": failed to save tab state: %s") % e.what();
    }
}

void PrinterManagerView::loadTabState()
{
    std::lock_guard<std::mutex> lock(s_tabStateMutex);
    try {
        std::string filePath = (boost::filesystem::path(Slic3r::data_dir()) / "user" / "printer_tab_state.json").string();
        
        if (!boost::filesystem::exists(filePath)) {
            return; // No saved state
        }
        
        boost::nowide::ifstream file(filePath);
        if (!file.is_open()) {
            return;
        }
        
        nlohmann::json tabState;
        file >> tabState;
        file.close();
        
        if (!tabState.is_object() || !tabState.contains("tabs")) {
            return;
        }
        
        nlohmann::json tabs = tabState["tabs"];
        if (!tabs.is_array()) {
            return;
        }
        
        // Load tabs in order
        for (const auto& tabInfo : tabs) {
            if (tabInfo.contains("printerId")) {
                std::string printerId = tabInfo["printerId"];
                openPrinterTab(printerId, false);
            }
        }
        int activeTabIndex = 0;
        // Restore active tab after all tabs are loaded
        if (tabState.contains("activeTabIndex")) {
            activeTabIndex = tabState["activeTabIndex"];
      
        }
        if (activeTabIndex >= 0 && activeTabIndex < mTabBar->GetPageCount()) {
            mTabBar->SetSelection(activeTabIndex);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": failed to load tab state: %s") % e.what();
    }
}


ElegooPrinterWebView* PrinterManagerView::findPrinterView(const std::string& printerId)
{
    std::lock_guard<std::mutex> lock(mPrinterViewsMutex);
    auto it = mPrinterViews.find(printerId);
    return (it != mPrinterViews.end()) ? it->second : nullptr;
}

void PrinterManagerView::insertPrinterView(const std::string& printerId, ElegooPrinterWebView* view)
{
    std::lock_guard<std::mutex> lock(mPrinterViewsMutex);
    mPrinterViews[printerId] = view;
}

bool PrinterManagerView::removePrinterView(const std::string& printerId)
{
    std::lock_guard<std::mutex> lock(mPrinterViewsMutex);
    return mPrinterViews.erase(printerId) > 0;
}

ElegooPrinterWebView* PrinterManagerView::removePrinterViewByWindow(wxWindow* win)
{
    std::lock_guard<std::mutex> lock(mPrinterViewsMutex);
    for (auto it = mPrinterViews.begin(); it != mPrinterViews.end(); ++it) {
        if (it->second == win) {
            ElegooPrinterWebView* view = it->second;
            mPrinterViews.erase(it);
            return view;
        }
    }
    return nullptr;
}

void PrinterManagerView::forEachPrinterView(std::function<void(const std::string&, ElegooPrinterWebView*)> callback)
{
    std::lock_guard<std::mutex> lock(mPrinterViewsMutex);
    for (const auto& pair : mPrinterViews) {
        callback(pair.first, pair.second);
    }
}

} // GUI
} // Slic3r


