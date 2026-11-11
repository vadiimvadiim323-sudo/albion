#pragma once
// FEATURE-V11-DIAG: A modeless, text-first status panel. It reads snapshots;
// device/model callbacks post commands; explicit local file dialogs/storage
// remain UI operations. They never hold the pipeline mutex.
#include <windows.h>
#include <objidl.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <gdiplus.h>
#include <functional>
#include <string>
#include "app_paths.hpp"
#include "ui_fonts.hpp"
namespace albion {
enum class DiagnosticAction {
    ReloadModel=1, RecoverCapture, OpenData, OpenHistory, RestoreConfig,
    ImportConfig, ExportConfig, Doctor, SupportPreview, OfflineCheck, ForgetKey
};
// FIX-V11.1-UI-STYLE: same dark palette as the settings window (kTh* values);
// 1.0.x used white system controls that clashed with the rest of the app.
namespace diagnostics_theme {
constexpr COLORREF kBg=RGB(0x11,0x12,0x17), kCard=RGB(0x1c,0x1d,0x23), kHover=RGB(0x2d,0x2f,0x38),
    kPressed=RGB(0x4d,0x15,0x15), kText=RGB(0xE0,0xE0,0xE0), kBright=RGB(255,255,255),
    kMuted=RGB(0xA0,0xA0,0xA0), kAccent=RGB(0xff,0x35,0x48), kBorder=RGB(0x30,0x31,0x3a);
inline Gdiplus::Color Gp(COLORREF c) { return Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c)); }
inline void RoundFill(HDC dc, RECT r, float d, COLORREF fill, COLORREF edge) {
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float x=r.left+0.5f, y=r.top+0.5f, w=float(r.right-r.left-1), h=float(r.bottom-r.top-1);
    d=(d<w?(d<h?d:h):w);
    Gdiplus::GraphicsPath path; path.AddArc(x,y,d,d,180.f,90.f); path.AddArc(x+w-d,y,d,d,270.f,90.f);
    path.AddArc(x+w-d,y+h-d,d,d,0.f,90.f); path.AddArc(x,y+h-d,d,d,90.f,90.f); path.CloseFigure();
    Gdiplus::SolidBrush brush(Gp(fill)); g.FillPath(&brush,&path);
    if(edge!=fill) { Gdiplus::Pen pen(Gp(edge),1.f); g.DrawPath(&pen,&path); }
}
} // namespace diagnostics_theme
class DiagnosticPanel {
    HWND window_=nullptr, text_=nullptr, pause_=nullptr, hovered_=nullptr; HFONT font_=nullptr, bold_=nullptr;
    HBRUSH bg_=nullptr, card_=nullptr; bool paused_=false;
    std::wstring last_; std::function<void(DiagnosticAction)> action_;
    static constexpr RECT kCardRc{12,12,762,348};
    static LRESULT CALLBACK HoverProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) noexcept {
        auto* self=reinterpret_cast<DiagnosticPanel*>(ref);
        if(self && m==WM_MOUSEMOVE && self->hovered_!=h) {
            if(self->hovered_ && IsWindow(self->hovered_)) InvalidateRect(self->hovered_,nullptr,FALSE);
            self->hovered_=h; TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0}; TrackMouseEvent(&t); InvalidateRect(h,nullptr,FALSE);
        }
        if(self && (m==WM_MOUSELEAVE || m==WM_NCDESTROY) && self->hovered_==h) { self->hovered_=nullptr; if(m==WM_MOUSELEAVE) InvalidateRect(h,nullptr,FALSE); }
        if(m==WM_SETCURSOR) { SetCursor(LoadCursor(nullptr,IDC_HAND)); return TRUE; }
        if(m==WM_LBUTTONDBLCLK) m=WM_LBUTTONDOWN; // fast repeated clicks are not lost
        if(m==WM_SETFOCUS || m==WM_KILLFOCUS) InvalidateRect(h,nullptr,FALSE);
        if(m==WM_NCDESTROY) RemoveWindowSubclass(h,&DiagnosticPanel::HoverProc,id);
        return DefSubclassProc(h,m,w,l);
    }
    void DrawItem(const DRAWITEMSTRUCT& d) const {
        using namespace diagnostics_theme;
        const RECT r=d.rcItem; const int w=r.right-r.left, h=r.bottom-r.top;
        HDC mem=CreateCompatibleDC(d.hDC); HBITMAP bmp=mem?CreateCompatibleBitmap(d.hDC,w,h):nullptr;
        HDC dc=(mem&&bmp)?mem:d.hDC; HGDIOBJ oldBmp=(mem&&bmp)?SelectObject(mem,bmp):nullptr;
        RECT rc=(mem&&bmp)?RECT{0,0,w,h}:r;
        FillRect(dc,&rc,bg_); SetBkMode(dc,TRANSPARENT);
        const bool hot=hovered_==d.hwndItem, down=(d.itemState&ODS_SELECTED)!=0, focus=(d.itemState&ODS_FOCUS)!=0;
        wchar_t label[96]{}; GetWindowTextW(d.hwndItem,label,95);
        if(d.hwndItem==pause_) {
            // Switch, as in the "Visuals" tab.
            const float sw=40.f, sh=22.f, sx=float(rc.left)+1.f, sy=(rc.top+rc.bottom)/2.f-sh/2.f;
            RECT track{int(sx),int(sy),int(sx+sw),int(sy+sh)};
            RoundFill(dc,track,sh,paused_?kAccent:(hot?kHover:kCard),focus?kAccent:(paused_?kAccent:kBorder));
            { Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
              Gdiplus::SolidBrush knob(Gp(paused_?kBright:kMuted)); const float kd=14.f;
              g.FillEllipse(&knob,paused_?sx+sw-kd-4.f:sx+4.f,sy+4.f,kd,kd); }
            RECT tr=rc; tr.left+=int(sw)+12;
            HGDIOBJ of=SelectObject(dc,font_); SetTextColor(dc,hot||paused_?kBright:kText);
            DrawTextW(dc,label,-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX); SelectObject(dc,of);
        } else {
            const bool danger=GetDlgCtrlID(d.hwndItem)==1011;
            const COLORREF fill=down?kPressed:(hot?kHover:kCard);
            RoundFill(dc,rc,10.f,fill,focus?kAccent:fill);
            HGDIOBJ of=SelectObject(dc,bold_?bold_:font_);
            SetTextColor(dc,danger?kAccent:(hot||down?kBright:kText));
            DrawTextW(dc,label,-1,&rc,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX); SelectObject(dc,of);
        }
        if(oldBmp) { BitBlt(d.hDC,r.left,r.top,w,h,mem,0,0,SRCCOPY); SelectObject(mem,oldBmp); }
        if(bmp) DeleteObject(bmp); if(mem) DeleteDC(mem);
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) noexcept {
        auto* self=reinterpret_cast<DiagnosticPanel*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE) { self=static_cast<DiagnosticPanel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); self->window_=h; SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
        try {
            // FEATURE-V11-ACCESS: never reuse IDOK/IDCANCEL for actions;
            // otherwise dialog navigation could turn Escape into recovery.
            if(self && m==WM_COMMAND && LOWORD(w)==IDCANCEL) { ShowWindow(h,SW_HIDE); return 0; }
            if(self && m==WM_COMMAND && LOWORD(w)==IDOK) {
                const auto focus=GetFocus(); const int id=focus?GetDlgCtrlID(focus):0;
                if(focus && GetParent(focus)==h && id>=1001 && id<=1011) SendMessageW(focus,BM_CLICK,0,0);
                return 0;
            }
            if(self && m==WM_COMMAND && HIWORD(w)==BN_CLICKED && LOWORD(w)==20) {
                self->paused_=!self->paused_; InvalidateRect(self->pause_,nullptr,FALSE); return 0;
            }
            if(self && m==WM_DRAWITEM && l) { self->DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(l)); return TRUE; }
            if(self && m==WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(l)==self->text_) { // read-only edit
                HDC dc=reinterpret_cast<HDC>(w); SetTextColor(dc,diagnostics_theme::kText); SetBkColor(dc,diagnostics_theme::kCard);
                return reinterpret_cast<LRESULT>(self->card_);
            }
            if(self && m==WM_ERASEBKGND) {
                RECT client{}; GetClientRect(h,&client); FillRect(reinterpret_cast<HDC>(w),&client,self->bg_);
                diagnostics_theme::RoundFill(reinterpret_cast<HDC>(w),kCardRc,14.f,diagnostics_theme::kCard,diagnostics_theme::kBorder);
                return 1;
            }
            if(self && m==WM_COMMAND && HIWORD(w)==BN_CLICKED && LOWORD(w)>=1001 && LOWORD(w)<=1011) {
                if(self->action_) self->action_(static_cast<DiagnosticAction>(LOWORD(w)-1000)); return 0;
            }
            if(self && m==WM_CLOSE) { ShowWindow(h,SW_HIDE); return 0; }
            if(self && m==WM_NCDESTROY) { self->window_=nullptr; self->text_=nullptr; self->pause_=nullptr; self->hovered_=nullptr; self->paused_=false; self->last_.clear(); SetWindowLongPtrW(h,GWLP_USERDATA,0); }
        } catch (...) { MessageBoxW(h,L"Ошибка диагностической операции. Проверьте состояние и папку данных перед повтором.",L"Диагностика",MB_ICONERROR); }
        return DefWindowProcW(h,m,w,l);
    }
public:
    ~DiagnosticPanel() { Close(); for(HGDIOBJ o : {static_cast<HGDIOBJ>(font_),static_cast<HGDIOBJ>(bold_),static_cast<HGDIOBJ>(bg_),static_cast<HGDIOBJ>(card_)}) if(o) DeleteObject(o); }
    void Close() noexcept { action_={}; if(window_) DestroyWindow(window_); window_=nullptr; text_=pause_=hovered_=nullptr; paused_=false; last_.clear(); }
    bool HandleMessage(MSG& message) {
        return window_ && (message.hwnd==window_ || IsChild(window_,message.hwnd)) && IsDialogMessageW(window_,&message);
    }
    void Show(HWND owner, std::function<void(DiagnosticAction)> action) {
        action_=std::move(action);
        if(window_) { ShowWindow(window_,SW_SHOWNORMAL); SetForegroundWindow(window_); return; }
        const HINSTANCE instance=GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc=Proc; wc.hInstance=instance;
        if(!bg_) bg_=CreateSolidBrush(diagnostics_theme::kBg);
        if(!card_) card_=CreateSolidBrush(diagnostics_theme::kCard);
        if(!font_) font_=AlbionFont(13,400);
        if(!bold_) bold_=AlbionFont(13,600);
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hbrBackground=bg_; wc.lpszClassName=L"AlbionDiagnosticsV2";
        if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return;
        window_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Диагностика — F8",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,790,550,owner,nullptr,instance,this);
        if(!window_) return;
        { const BOOL dark=TRUE; DwmSetWindowAttribute(window_,20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,&dark,sizeof(dark)); }
        text_=CreateWindowExW(0,L"EDIT",L"Ожидание снимка состояния...",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY,
            24,22,728,318,window_,nullptr,instance,nullptr);
        if(text_) SetWindowTheme(text_,L"DarkMode_Explorer",nullptr); // dark scrollbar
        const wchar_t* labels[]={L"Модель: повтор",L"Захват: восстановить",L"Папка данных",L"История конфигов",L"Восстановить конфиг",
            L"Импорт конфига",L"Экспорт настроек",L"Проверить окружение",L"Пакет для поддержки",L"Проверить ONNX",L"Забыть ключ"};
        bool controlsOk=text_!=nullptr;
        for(int i=0;i<11;++i) {
            const auto control=CreateWindowExW(0,L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                12+(i%4)*188,360+(i/4)*40,180,32,window_,reinterpret_cast<HMENU>(INT_PTR(1001+i)),instance,nullptr);
            controlsOk=controlsOk && control!=nullptr;
            if(control) SetWindowSubclass(control,&DiagnosticPanel::HoverProc,1,reinterpret_cast<DWORD_PTR>(this));
        }
        pause_=CreateWindowExW(0,L"BUTTON",L"Пауза обновления текста",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,12,482,400,26,window_,reinterpret_cast<HMENU>(INT_PTR(20)),instance,nullptr);
        if(pause_) SetWindowSubclass(pause_,&DiagnosticPanel::HoverProc,1,reinterpret_cast<DWORD_PTR>(this));
        if(!controlsOk || !pause_) { Close(); MessageBoxW(owner,L"Не удалось создать элементы панели диагностики.",L"Диагностика",MB_ICONERROR); return; }
        last_.clear(); paused_=false;
        for(HWND c=GetWindow(window_,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT)) SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        ShowWindow(window_,SW_SHOW); SetForegroundWindow(window_);
    }
    void Update(const std::wstring& text) {
        if(window_ && text_ && text!=last_ && !paused_) {
            SetWindowTextW(text_,text.c_str()); last_=text;
        }
    }
};
} // namespace albion
