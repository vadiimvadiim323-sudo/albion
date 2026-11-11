#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <commctrl.h>
#include <auth.hpp>
#include "auth_gate.hpp"
#include "license_key.hpp"
#include "license_cache.hpp"
#include "ui_fonts.hpp"
#include "Resource.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
namespace {
constexpr int kEdit=101, kSubmit=102, kStatus=103, kShow=104, kMinimize=105, kClose=106, kRemember=107, kForget=108;
constexpr UINT_PTR kTimer=1001, kFadeTimer=1002, kDotsTimer=1003, kSuccessTimer=1004;
// Same size, frame and grid as the settings window (820x540, sidebar 0..92, content 124..788).
constexpr int kWidth=820, kHeight=540, kL=124, kR=788, kCorner=18;
constexpr int kCardL=246, kCardR=666;
// Same palette as the main window (kTh* in WindowsProject1.cpp).
// kBg=kThPanel, kText=kThText, kBright=kThTextBright, kMuted=kThTextMuted,
// kEditBg=kThInput, kHover=kThSelect, kAccent=kThAccent.
constexpr COLORREF kBg=RGB(0x11,0x12,0x17), kText=RGB(0xE0,0xE0,0xE0), kBright=RGB(255,255,255),
    kMuted=RGB(0xA0,0xA0,0xA0), kIdle=RGB(104,105,114),
    kAccent=RGB(0xff,0x35,0x48), kAccentHot=RGB(255,82,98), kAccentDown=RGB(205,36,52),
    kEditBg=RGB(0x1c,0x1d,0x23), kLine=RGB(0x1c,0x1d,0x23), kHover=RGB(0x2d,0x2f,0x38),
    kInputBorder=RGB(58,59,68), kBorder=RGB(0xff,0x35,0x48), kOk=RGB(0,200,90);
// Layout (client coordinates).
constexpr RECT kInputRc{kCardL,262,kCardR,314};
constexpr RECT kStatusRc{kCardL+2,324,kCardR,370};
constexpr RECT kSubmitRc{kCardL,382,kCardR,436};
Gdiplus::Color gp(COLORREF c, BYTE a=255) { return Gdiplus::Color(a,GetRValue(c),GetGValue(c),GetBValue(c)); }
void roundPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float d) {
    d=std::min(d,std::min(w,h));
    path.AddArc(x,y,d,d,180.f,90.f); path.AddArc(x+w-d,y,d,d,270.f,90.f);
    path.AddArc(x+w-d,y+h-d,d,d,0.f,90.f); path.AddArc(x,y+h-d,d,d,90.f,90.f); path.CloseFigure();
}
// Antialiased rounded rectangle; radius is the corner diameter, like RoundRect.
void rounded(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path; roundPath(path,r.left+0.5f,r.top+0.5f,float(r.right-r.left-1),float(r.bottom-r.top-1),float(radius));
    Gdiplus::SolidBrush brush(gp(fill)); g.FillPath(&brush,&path);
    if(border!=fill) { Gdiplus::Pen pen(gp(border),1.f); g.DrawPath(&pen,&path); }
}
void line(HDC dc, float x1, float y1, float x2, float y2, COLORREF c, float width) {
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(gp(c),width); pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen,x1,y1,x2,y2);
}
COLORREF mix(COLORREF a, COLORREF b, float t) {
    auto ch=[t](int x,int y){ return static_cast<BYTE>(x+(y-x)*t+0.5f); };
    return RGB(ch(GetRValue(a),GetRValue(b)),ch(GetGValue(a),GetGValue(b)),ch(GetBValue(a),GetBValue(b)));
}
// ---- Hand-off: after a valid key the license screen is frozen into a
// click-through layered snapshot that stays on top until the settings window
// is shown at the same place, then cross-fades out (the frames match, so only
// the content "flows" into the settings page).
struct Handoff {
    HWND wnd{}; HDC dc{}; HBITMAP bmp{}; HGDIOBJ old{}; RECT rc{}; ULONGLONG start{}; bool fading=false;
} g_handoff;
constexpr UINT_PTR kHandoffTimer=1, kHandoffSafety=2; constexpr double kHandoffMs=450;
void handoffPush(BYTE alpha) {
    if(!g_handoff.wnd) return;
    BLENDFUNCTION bf{AC_SRC_OVER,0,alpha,AC_SRC_ALPHA};
    POINT dst{g_handoff.rc.left,g_handoff.rc.top}, src{0,0}; SIZE sz{g_handoff.rc.right-g_handoff.rc.left,g_handoff.rc.bottom-g_handoff.rc.top};
    UpdateLayeredWindow(g_handoff.wnd,nullptr,&dst,&sz,g_handoff.dc,&src,0,&bf,ULW_ALPHA);
}
void handoffRelease() {
    if(g_handoff.dc) { if(g_handoff.old)SelectObject(g_handoff.dc,g_handoff.old); DeleteDC(g_handoff.dc); }
    if(g_handoff.bmp) DeleteObject(g_handoff.bmp);
    g_handoff=Handoff{};
}
LRESULT CALLBACK HandoffProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if(w==kHandoffSafety) { KillTimer(h,kHandoffSafety); AlbionAuthFinishHandoff(nullptr); return 0; }
        if(w==kHandoffTimer) {
            const double t=std::min(1.0,double(GetTickCount64()-g_handoff.start)/kHandoffMs);
            const double e=t<0.5?2*t*t:1-(-2*t+2)*(-2*t+2)/2;
            if(t>=1.0) { DestroyWindow(h); return 0; }
            handoffPush(static_cast<BYTE>(255.0*(1.0-e)+0.5)); return 0;
        }
        break;
    case WM_DESTROY: KillTimer(h,kHandoffTimer); KillTimer(h,kHandoffSafety); if(g_handoff.wnd==h){ g_handoff.wnd=nullptr; handoffRelease(); } return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
// Snapshot of the whole gate window (children included), corners cut like the window region.
bool handoffCreate(HINSTANCE instance, HWND gate) {
    if (AlbionReducedMotion()) return false;
    RECT wr{}; if(!GetWindowRect(gate,&wr)) return false;
    const int w=wr.right-wr.left, hgt=wr.bottom-wr.top; if(w<=0||hgt<=0) return false;
    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(bi.bmiHeader); bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-hgt;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    HDC screen=GetDC(nullptr); void* bits=nullptr;
    HDC dc=CreateCompatibleDC(screen); HBITMAP bmp=dc?CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
    void* maskBits=nullptr; HDC mdc=CreateCompatibleDC(screen); HBITMAP mbmp=mdc?CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&maskBits,nullptr,0):nullptr;
    bool ok=dc&&bmp&&bits&&mdc&&mbmp&&maskBits;
    HGDIOBJ old=ok?SelectObject(dc,bmp):nullptr;
    if(ok) {
        RedrawWindow(gate,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
        if(!PrintWindow(gate,dc,2 /*PW_RENDERFULLCONTENT*/)) BitBlt(dc,0,0,w,hgt,screen,wr.left,wr.top,SRCCOPY);
        HGDIOBJ mold=SelectObject(mdc,mbmp); RECT all{0,0,w,hgt}; FillRect(mdc,&all,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        HRGN rgn=CreateRoundRectRgn(0,0,w+1,hgt+1,kCorner,kCorner); if(rgn){ FillRgn(mdc,rgn,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH))); DeleteObject(rgn); }
        GdiFlush(); SelectObject(mdc,mold);
        auto* px=static_cast<UINT32*>(bits); const auto* mk=static_cast<const UINT32*>(maskBits);
        for(size_t i=0,n=size_t(w)*size_t(hgt);i<n;++i) px[i]=(mk[i]&0xFF)?(px[i]|0xFF000000u):0u;
    }
    if(mbmp)DeleteObject(mbmp);
    if(mdc)DeleteDC(mdc);
    if(screen)ReleaseDC(nullptr,screen);
    if(!ok) { if(dc){ if(old)SelectObject(dc,old); DeleteDC(dc);} if(bmp)DeleteObject(bmp); return false; }
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc=HandoffProc; wc.hInstance=instance; wc.lpszClassName=L"AlbionAuthHandoff";
    if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) { SelectObject(dc,old); DeleteDC(dc); DeleteObject(bmp); return false; }
    g_handoff.dc=dc; g_handoff.bmp=bmp; g_handoff.old=old; g_handoff.rc=wr;
    g_handoff.wnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,wc.lpszClassName,L"",WS_POPUP,
        wr.left,wr.top,w,hgt,nullptr,nullptr,instance,nullptr);
    if(!g_handoff.wnd) { handoffRelease(); return false; }
    handoffPush(255);
    SetWindowPos(g_handoff.wnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    SetTimer(g_handoff.wnd,kHandoffSafety,60000,nullptr); // never stays forever if the app fails to start
    return true;
}
struct Job { std::atomic<int> state{0}; std::atomic<bool> cancelled{false}, cacheFailed{false}; };
struct Gate {
    HINSTANCE instance{}; HWND window{}, edit{}, submit{}, status{}, show{}, minimize{}, close{}, remember{}, forget{};
    HFONT heading{}, body{}, button{}, brand{}, smallFont{}, input{}, tab{}; HBRUSH bg{}, editBg{};
    std::unique_ptr<Gdiplus::Image> logoImg, authImg;
    std::shared_ptr<Job> job; HWND hovered{}; bool accepted=false, done=false, visible=false;
    bool rememberKey=true; // FIX-V11.1-AUTH-STYLE: owner-drawn checkbox state
    RECT inputRc=kInputRc; int statusKind=0; // 0 muted, 1 progress, 2 error, 3 success
    int fadeStep=0, dots=0;
    ~Gate() { if(job) job->cancelled = true; logoImg.reset(); authImg.reset(); HGDIOBJ objs[]={heading,body,button,brand,smallFont,input,tab,bg,editBg}; for(HGDIOBJ o:objs) if(o)DeleteObject(o); }
    void setStatus(int kind, const wchar_t* text) {
        statusKind=kind; SetWindowTextW(status,text);
        if(window) { RECT r=kStatusRc; InvalidateRect(window,&r,TRUE); InvalidateRect(submit,nullptr,FALSE); }
        if(window && kind==3) { RECT r=inputRc; InvalidateRect(window,&r,FALSE); }
        if(window && kind==1) { dots=0; SetTimer(window,kDotsTimer,350,nullptr); }
        else if(window) KillTimer(window,kDotsTimer);
    }
    void startRequest() {
        if(job) return;
        wchar_t text[160]{}; GetWindowTextW(edit,text,160);
        std::string raw; for (const wchar_t* p=text; *p; ++p) { if (*p>127) { setStatus(2,L"\u041a\u043b\u044e\u0447 \u0434\u043e\u043b\u0436\u0435\u043d \u0441\u043e\u0434\u0435\u0440\u0436\u0430\u0442\u044c \u043b\u0430\u0442\u0438\u043d\u0441\u043a\u0438\u0435 \u0441\u0438\u043c\u0432\u043e\u043b\u044b."); SecureZeroMemory(text,sizeof(text)); return; } raw.push_back(static_cast<char>(*p)); }
        SecureZeroMemory(text,sizeof(text));
        std::string key;
        const bool valid=NormalizeAlbionLicense(raw,key);
        if(!raw.empty()) SecureZeroMemory(&raw[0],raw.size());
        if(!valid) { setStatus(2,L"\u041f\u0440\u043e\u0432\u0435\u0440\u044c\u0442\u0435 \u0444\u043e\u0440\u043c\u0430\u0442 \u043a\u043b\u044e\u0447\u0430: ALBION-..."); return; }
        job=std::make_shared<Job>(); auto state=job;
        const bool saveKey = rememberKey;
        EnableWindow(remember,FALSE); EnableWindow(forget,FALSE);
        EnableWindow(submit,FALSE); EnableWindow(edit,FALSE); InvalidateRect(window,&inputRc,FALSE);
        setStatus(1,L"\u041f\u0440\u043e\u0432\u0435\u0440\u043a\u0430 \u043b\u0438\u0446\u0435\u043d\u0437\u0438\u0438... \u041d\u0443\u0436\u0435\u043d \u0438\u043d\u0442\u0435\u0440\u043d\u0435\u0442.");
        try {
            std::thread([state,saveKey,key=std::move(key)]() mutable {
                int result=3;
                try {
                    KeyAuth::api api("AlbionAI", "Uzwx5EZ5sn", "1.0", "https://keyauth.win/api/1.3/", "");
                    api.init();
                    if(api.response.success) {
                        api.license(key);
                        result=(api.response.success && api.authenticated()) ? 1 : 2;
                    }
                } catch (...) { result=3; }
                // FEATURE-V11-KEY: only a successful, still-requested online
                // login may update the cache. Network errors do not erase it.
                if (result==1 && !state->cancelled.load()) {
                    const bool stored = saveKey ? albion::SaveLicenseCache(key) : albion::ForgetLicenseCache();
                    state->cacheFailed.store(!stored);
                }
                if(!key.empty()) SecureZeroMemory(&key[0],key.size());
                state->state.store(result,std::memory_order_release);
            }).detach();
        } catch (...) {
            if(!key.empty()) SecureZeroMemory(&key[0],key.size());
            job.reset(); EnableWindow(submit,TRUE); EnableWindow(edit,TRUE); EnableWindow(remember,TRUE); EnableWindow(forget,TRUE);
            setStatus(2,L"\u041d\u0435 \u0443\u0434\u0430\u043b\u043e\u0441\u044c \u043d\u0430\u0447\u0430\u0442\u044c \u043f\u0440\u043e\u0432\u0435\u0440\u043a\u0443.");
        }
    }
    static LRESULT CALLBACK ChildVisualProc(HWND child, UINT m, WPARAM w, LPARAM l,
        UINT_PTR id, DWORD_PTR ref) {
        auto* self=reinterpret_cast<Gate*>(ref);
        if(self && (m==WM_SETFOCUS || m==WM_KILLFOCUS) && child==self->edit)
            InvalidateRect(GetParent(child),&self->inputRc,FALSE);
        if(self && child!=self->edit && m==WM_MOUSEMOVE && self->hovered!=child) {
            HWND previous=self->hovered; self->hovered=child;
            if(previous && IsWindow(previous)) InvalidateRect(previous,nullptr,FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,child,0}; TrackMouseEvent(&tracking);
            InvalidateRect(child,nullptr,FALSE);
        }
        if(self && (m==WM_MOUSELEAVE || m==WM_NCDESTROY) && self->hovered==child) {
            self->hovered=nullptr; if(m==WM_MOUSELEAVE)InvalidateRect(child,nullptr,FALSE);
        }
        if(self && child!=self->edit && m==WM_SETCURSOR) { SetCursor(LoadCursor(nullptr,IDC_HAND)); return TRUE; }
        if(self && child!=self->edit && m==WM_LBUTTONDBLCLK) m=WM_LBUTTONDOWN; // no lost clicks
        if(self && child==self->edit && m==WM_PAINT) { // placeholder works without comctl32 v6
            const LRESULT r=DefSubclassProc(child,m,w,l);
            if(GetWindowTextLengthW(child)==0) {
                HDC dc=GetDC(child); RECT rc{}; GetClientRect(child,&rc);
                HGDIOBJ of=SelectObject(dc,self->input); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,kIdle);
                DrawTextW(dc,L"ALBION-XXXXX-XXXXX-XXXXX",-1,&rc,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                SelectObject(dc,of); ReleaseDC(child,dc);
            }
            return r;
        }
        if(self && child==self->edit && m==WM_KEYDOWN && w=='A' && (GetKeyState(VK_CONTROL)&0x8000)) {
            SendMessageW(child,EM_SETSEL,0,-1); return 0; // Ctrl+A
        }
        if(self && child==self->edit && m==WM_CHAR && w==1) return 0; // no beep after Ctrl+A
        if(m==WM_NCDESTROY)RemoveWindowSubclass(child,&Gate::ChildVisualProc,id);
        return DefSubclassProc(child,m,w,l);
    }
    void drawEye(HDC dc, RECT r, COLORREF c, bool open) {
        const float cx=(r.left+r.right)/2.f, cy=(r.top+r.bottom)/2.f;
        Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(gp(c),1.8f); pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        Gdiplus::GraphicsPath eye;
        eye.AddBezier(cx-10.f,cy,cx-5.f,cy-7.f,cx+5.f,cy-7.f,cx+10.f,cy);
        eye.AddBezier(cx+10.f,cy,cx+5.f,cy+7.f,cx-5.f,cy+7.f,cx-10.f,cy);
        eye.CloseFigure(); g.DrawPath(&pen,&eye);
        Gdiplus::SolidBrush pupil(gp(c)); g.FillEllipse(&pupil,cx-2.8f,cy-2.8f,5.6f,5.6f);
        if(!open) { Gdiplus::Pen cut(gp(c),1.8f); cut.SetStartCap(Gdiplus::LineCapRound); cut.SetEndCap(Gdiplus::LineCapRound); g.DrawLine(&cut,cx-8.f,cy+7.f,cx+8.f,cy-7.f); }
    }
    void paintWindow(HDC dc) {
        RECT client{}; GetClientRect(window,&client); FillRect(dc,&client,bg);
        // Frame identical to PaintSettingsBackground in the settings window.
        HBRUSH lineBr=CreateSolidBrush(kLine);
        RECT divider{92,24,93,client.bottom-24}; FillRect(dc,&divider,lineBr);
        RECT logoLine{26,98,66,99}; FillRect(dc,&logoLine,lineBr);
        RECT bottomLine{18,client.bottom-92,74,client.bottom-91}; FillRect(dc,&bottomLine,lineBr);
        DeleteObject(lineBr);
        SetBkMode(dc,TRANSPARENT);
        if(logoImg) { // same albionai.png and box (22,28,70,76) as the settings window
            Gdiplus::Graphics g(dc); g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            const float iw=float(logoImg->GetWidth()), ih=float(logoImg->GetHeight());
            const float k=std::min(48.f/std::max(iw,1.f),48.f/std::max(ih,1.f));
            g.DrawImage(logoImg.get(),Gdiplus::RectF(46.f-iw*k/2.f,52.f-ih*k/2.f,iw*k,ih*k));
        } else {
            HICON icon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_WINDOWSPROJECT1),IMAGE_ICON,44,44,LR_SHARED));
            if(icon) DrawIconEx(dc,24,30,icon,44,44,0,nullptr,DI_NORMAL);
        }
        // Sidebar: one active item in the first nav slot (24,124,44x44) with extras\images\auth.png.
        { RECT slot{24,124,68,168}; rounded(dc,slot,12,kEditBg,kEditBg);
          if(authImg) {
            Gdiplus::Graphics g(dc); g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            const float iw=float(authImg->GetWidth()), ih=float(authImg->GetHeight());
            const float k=std::min(24.f/std::max(iw,1.f),24.f/std::max(ih,1.f));
            g.DrawImage(authImg.get(),Gdiplus::RectF(46.f-iw*k/2.f,146.f-ih*k/2.f,iw*k,ih*k));
          } else {
            const float cx=46.f, cy=148.f; const COLORREF c=statusKind==3?kOk:kBright;
            { Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
              Gdiplus::Pen shackle(gp(c),2.f); g.DrawArc(&shackle,cx-6.f,cy-13.f,12.f,12.f,180.f,180.f);
              g.DrawLine(&shackle,cx-6.f,cy-7.f,cx-6.f,cy-2.f); g.DrawLine(&shackle,cx+6.f,cy-7.f,cx+6.f,cy-2.f); }
            RECT lockBody{int(cx)-9,int(cy)-2,int(cx)+10,int(cy)+11}; rounded(dc,lockBody,6,c,c);
          } }
        // Sidebar corner: state dot like the settings window.
        { const COLORREF sc=statusKind==3?kOk:kAccent; HGDIOBJ of=SelectObject(dc,button);
          const wchar_t* v=statusKind==3?L"OK":L"\u041a\u043b\u044e\u0447"; SIZE ext{}; GetTextExtentPoint32W(dc,v,lstrlenW(v),&ext);
          const int rowW=14+ext.cx, x0=46-rowW/2, y=client.bottom-74;
          { Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush dot(gp(sc)); g.FillEllipse(&dot,float(x0),float(y+6),8.f,8.f); }
          RECT st{x0+14,y,x0+16+ext.cx,y+20}; SetTextColor(dc,sc); DrawTextW(dc,v,-1,&st,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
          SelectObject(dc,of); }
        // Page title where the settings window shows the tab title.
        HGDIOBJ oldFont=SelectObject(dc,tab); SetTextColor(dc,kBright);
        RECT pageTitle{kL,24,kR-80,64}; DrawTextW(dc,L"\u041b\u0438\u0446\u0435\u043d\u0437\u0438\u044f",-1,&pageTitle,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        // Centered card.
        SelectObject(dc,brand); RECT b{kCardL,110,kCardR,156}; SIZE s1{},s2{};
        GetTextExtentPoint32W(dc,L"Albion",6,&s1); GetTextExtentPoint32W(dc,L"AI",2,&s2);
        const int bx=(kCardL+kCardR-(s1.cx+s2.cx))/2; b.left=bx; SetTextColor(dc,kBright); DrawTextW(dc,L"Albion",-1,&b,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        b.left=bx+s1.cx; SetTextColor(dc,kAccent); DrawTextW(dc,L"AI",-1,&b,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        SelectObject(dc,heading); SetTextColor(dc,kBright); RECT title{kCardL-40,172,kCardR+40,210};
        DrawTextW(dc,statusKind==3?L"\u041b\u0438\u0446\u0435\u043d\u0437\u0438\u044f \u043f\u043e\u0434\u0442\u0432\u0435\u0440\u0436\u0434\u0435\u043d\u0430":L"\u0410\u043a\u0442\u0438\u0432\u0430\u0446\u0438\u044f \u043b\u0438\u0446\u0435\u043d\u0437\u0438\u0438",-1,&title,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
        SelectObject(dc,smallFont); SetTextColor(dc,kMuted); RECT label{kCardL+2,238,kCardR,258};
        DrawTextW(dc,L"\u041a\u041b\u042e\u0427 \u041b\u0418\u0426\u0415\u041d\u0417\u0418\u0418",-1,&label,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        // Input pill: accent border on focus, green after success.
        const bool focused=GetFocus()==edit, locked=!IsWindowEnabled(edit);
        rounded(dc,inputRc,14,kEditBg,statusKind==3?kOk:(focused?kAccent:(locked?kEditBg:kInputBorder)));
        if(focused) { // soft accent halo around the focused field
            Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath halo; roundPath(halo,inputRc.left-1.5f,inputRc.top-1.5f,float(inputRc.right-inputRc.left+2),float(inputRc.bottom-inputRc.top+2),17.f);
            Gdiplus::Pen pen(gp(kAccent,70),2.f); g.DrawPath(&pen,&halo); }
        { const float cx=inputRc.left+24.f, cy=(inputRc.top+inputRc.bottom)/2.f; const COLORREF c=statusKind==3?kOk:(focused?kAccent:kMuted);
          Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
          Gdiplus::Pen pen(gp(c),1.8f); pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
          g.DrawEllipse(&pen,cx-8.f,cy-7.f,9.f,9.f);            // key bow
          g.DrawLine(&pen,cx-0.5f,cy+0.5f,cx+7.f,cy+8.f);        // blade
          g.DrawLine(&pen,cx+4.5f,cy+5.5f,cx+7.f,cy+3.f); }      // tooth
        SelectObject(dc,oldFont);
        // Rounded accent frame, as in the settings window.
        HPEN pen=CreatePen(PS_SOLID,1,kBorder); HGDIOBJ op=SelectObject(dc,pen), ob=SelectObject(dc,GetStockObject(NULL_BRUSH));
        RoundRect(dc,0,0,client.right-1,client.bottom-1,kCorner,kCorner); SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(pen);
    }
    LRESULT proc(HWND h,UINT m,WPARAM w,LPARAM l) {
        switch(m) {
        case WM_CREATE: {
            window=h;
            bg=CreateSolidBrush(kBg); editBg=CreateSolidBrush(kEditBg);
            heading=AlbionFont(26,700); body=AlbionFont(14,500); button=AlbionFont(15,700);
            brand=AlbionFont(28,700); smallFont=AlbionFont(13,500); input=AlbionFont(15,600); tab=AlbionFont(20,700);
            { auto img=std::make_unique<Gdiplus::Image>((AlbionExeDir()+L"extras\\images\\albionai.png").c_str(),FALSE); if(img->GetLastStatus()==Gdiplus::Ok) logoImg=std::move(img); }
            { auto img=std::make_unique<Gdiplus::Image>((AlbionExeDir()+L"extras\\images\\auth.png").c_str(),FALSE); if(img->GetLastStatus()==Gdiplus::Ok) authImg=std::move(img); }
            const int editH=22, editY=(kInputRc.top+kInputRc.bottom-editH)/2;
            edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL|ES_PASSWORD,kInputRc.left+46,editY,kInputRc.right-kInputRc.left-46-52,editH,h,(HMENU)(INT_PTR)kEdit,instance,nullptr);
            SendMessageW(edit,EM_LIMITTEXT,136,0);
            SendMessageW(edit,EM_SETPASSWORDCHAR,0x2022,0);
            show=CreateWindowExW(0,L"BUTTON",L"\u041f\u043e\u043a\u0430\u0437\u0430\u0442\u044c \u043a\u043b\u044e\u0447",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,kInputRc.right-46,kInputRc.top+8,36,kInputRc.bottom-kInputRc.top-16,h,(HMENU)(INT_PTR)kShow,instance,nullptr);
            status=CreateWindowExW(0,L"STATIC",L"\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u043a\u043b\u044e\u0447 \u0434\u043b\u044f \u043f\u0440\u043e\u0432\u0435\u0440\u043a\u0438.",WS_CHILD|WS_VISIBLE,kStatusRc.left,kStatusRc.top,kStatusRc.right-kStatusRc.left,kStatusRc.bottom-kStatusRc.top,h,(HMENU)(INT_PTR)kStatus,instance,nullptr);
            submit=CreateWindowExW(0,L"BUTTON",L"\u0412\u043e\u0439\u0442\u0438",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,kSubmitRc.left,kSubmitRc.top,kSubmitRc.right-kSubmitRc.left,kSubmitRc.bottom-kSubmitRc.top,h,(HMENU)(INT_PTR)kSubmit,instance,nullptr);
            minimize=CreateWindowExW(0,L"BUTTON",L"\u0421\u0432\u0435\u0440\u043d\u0443\u0442\u044c",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,kR-70,26,32,32,h,(HMENU)(INT_PTR)kMinimize,instance,nullptr);
            close=CreateWindowExW(0,L"BUTTON",L"\u0417\u0430\u043a\u0440\u044b\u0442\u044c",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,kR-32,26,32,32,h,(HMENU)(INT_PTR)kClose,instance,nullptr);
            // Default checked as requested; the cached key is still verified
            // online on EVERY launch. Never store plaintext in config/logs.
            remember=CreateWindowExW(0,L"BUTTON",L"Запомнить ключ на этом Windows-аккаунте",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,kCardL,450,330,24,h,(HMENU)(INT_PTR)kRemember,instance,nullptr);
            forget=CreateWindowExW(0,L"BUTTON",L"Забыть",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,kCardR-84,447,84,30,h,(HMENU)(INT_PTR)kForget,instance,nullptr);
            // FIX-V11.1-AUTH-STYLE: both controls are owner-drawn in the card
            // style (rounded dark input + accent), not system Win32 controls.
            std::string cached;
            if (albion::LoadLicenseCache(cached)) {
                if (cached.compare(0,8,"KEYAUTH-")==0) cached.erase(0,8);
                std::wstring shown(cached.begin(),cached.end()); SetWindowTextW(edit,shown.c_str());
                if(!shown.empty()) SecureZeroMemory(shown.data(),shown.size()*sizeof(wchar_t));
                if(!cached.empty()) SecureZeroMemory(cached.data(),cached.size());
            }
            for(HWND c=GetWindow(h,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT)) SendMessageW(c,WM_SETFONT,(WPARAM)body,TRUE);
            SendMessageW(edit,WM_SETFONT,(WPARAM)input,TRUE); SendMessageW(submit,WM_SETFONT,(WPARAM)button,TRUE);
            SendMessageW(edit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(0,0));
            if(!bg || !editBg || !heading || !body || !button || !brand || !smallFont || !input || !tab || !edit || !submit || !status || !show || !minimize || !close || !remember || !forget || !SetTimer(h,kTimer,30,nullptr)) return -1;
            HWND styled[] = {edit,submit,show,minimize,close,remember,forget};
            for(HWND c : styled) SetWindowSubclass(c,&Gate::ChildVisualProc,1,reinterpret_cast<DWORD_PTR>(this));
            return 0;
        }
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; ScreenToClient(h,&pt);
            if(pt.y<76 && pt.x<kR-74) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_LBUTTONDOWN: { // click anywhere on the pill focuses the edit
            POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if(PtInRect(&inputRc,pt) && IsWindowEnabled(edit)) SetFocus(edit);
            return 0;
        }
        case WM_ACTIVATE:
            if(LOWORD(w)!=WA_INACTIVE && !HIWORD(w) && edit && IsWindowEnabled(edit)) { SetFocus(edit); return 0; }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps); RECT client{}; GetClientRect(h,&client);
            HDC mem=CreateCompatibleDC(dc); HBITMAP bmp=CreateCompatibleBitmap(dc,client.right,client.bottom);
            if(mem && bmp) { HGDIOBJ ob=SelectObject(mem,bmp); paintWindow(mem); BitBlt(dc,0,0,client.right,client.bottom,mem,0,0,SRCCOPY); SelectObject(mem,ob); }
            else paintWindow(dc);
            if(bmp)DeleteObject(bmp); if(mem)DeleteDC(mem);
            EndPaint(h,&ps); return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc=(HDC)w;
            if((HWND)l==edit) { SetTextColor(dc,kMuted); SetBkColor(dc,kEditBg); return (LRESULT)editBg; } // disabled edit
            SetBkColor(dc,kBg);
            if((HWND)l==status) SetTextColor(dc,statusKind==2?kAccentHot:(statusKind==3?kOk:(statusKind==1?kText:kMuted))); else SetTextColor(dc,kMuted);
            return (LRESULT)bg;
        }
        case WM_CTLCOLOREDIT: { HDC dc=(HDC)w; SetTextColor(dc,kText); SetBkColor(dc,kEditBg); return (LRESULT)editBg; }
        case WM_DRAWITEM: {
            auto d=(DRAWITEMSTRUCT*)l; if(!d) break; const int id=int(d->CtlID);
            if(id!=kSubmit && id!=kShow && id!=kMinimize && id!=kClose && id!=kRemember && id!=kForget) break;
            RECT r=d->rcItem; const bool hot=hovered==d->hwndItem, down=(d->itemState&ODS_SELECTED)!=0;
            HDC mem=CreateCompatibleDC(d->hDC); HBITMAP bmp=CreateCompatibleBitmap(d->hDC,r.right,r.bottom);
            HDC dc=(mem&&bmp)?mem:d->hDC; HGDIOBJ oldBmp=(mem&&bmp)?SelectObject(mem,bmp):nullptr;
            FillRect(dc,&r,id==kShow?editBg:bg); SetBkMode(dc,TRANSPARENT);
            if(id==kSubmit) {
                const bool busy=(d->itemState&ODS_DISABLED)!=0;
                const bool ok=statusKind==3;
                COLORREF color=ok?kAccent:(busy?RGB(110,34,44):(down?kAccentDown:(hot?kAccentHot:kAccent)));
                RECT pill=r; if(down) InflateRect(&pill,-1,-1);
                rounded(dc,pill,16,color,color);
                if(d->itemState&ODS_FOCUS && !busy) { RECT ring=r; HGDIOBJ ob=SelectObject(dc,GetStockObject(NULL_BRUSH)); HPEN p=CreatePen(PS_SOLID,1,RGB(255,160,170)); HGDIOBJ op=SelectObject(dc,p); RoundRect(dc,ring.left,ring.top,ring.right,ring.bottom,16,16); SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(p); }
                wchar_t label[64]=L"\u0412\u043e\u0439\u0442\u0438";
                if(ok) lstrcpyW(label,L"\u0413\u043e\u0442\u043e\u0432\u043e  \u2713");
                else if(busy) { lstrcpyW(label,L"\u041f\u0440\u043e\u0432\u0435\u0440\u043a\u0430"); for(int i=0;i<3;++i) lstrcatW(label,i<=dots%3?L".":L" "); }
                auto old=SelectObject(dc,button); SetTextColor(dc,(busy&&!ok)?RGB(225,200,204):kBright);
                DrawTextW(dc,label,-1,&pill,DT_CENTER|DT_VCENTER|DT_SINGLELINE); SelectObject(dc,old);
            } else if(id==kRemember) {
                // Rounded check box: accent fill + white tick when on, dark well otherwise.
                const bool disabled=(d->itemState&ODS_DISABLED)!=0, focus=(d->itemState&ODS_FOCUS)!=0;
                const int box=18, by=(r.top+r.bottom-box)/2;
                RECT boxRc{r.left,by,r.left+box,by+box};
                const COLORREF fill=rememberKey?(disabled?RGB(110,34,44):(hot?kAccentHot:kAccent)):(hot&&!disabled?kHover:kEditBg);
                const COLORREF edge=rememberKey?fill:(focus?kAccent:(hot&&!disabled?kMuted:kInputBorder));
                rounded(dc,boxRc,7,fill,edge);
                if(focus && rememberKey) { RECT ring=boxRc; InflateRect(&ring,2,2); Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::GraphicsPath path; roundPath(path,ring.left+0.5f,ring.top+0.5f,float(ring.right-ring.left-1),float(ring.bottom-ring.top-1),10.f);
                    Gdiplus::Pen pen(gp(kAccent,110),1.f); g.DrawPath(&pen,&path); }
                if(rememberKey) {
                    const float x=float(boxRc.left), y=float(boxRc.top);
                    const COLORREF tick=disabled?RGB(225,200,204):kBright;
                    line(dc,x+4.5f,y+9.5f,x+7.5f,y+12.5f,tick,2.f); line(dc,x+7.5f,y+12.5f,x+13.5f,y+5.5f,tick,2.f);
                }
                RECT tr=r; tr.left+=box+10;
                auto old=SelectObject(dc,body); SetTextColor(dc,disabled?kIdle:(hot||focus?kText:kMuted));
                DrawTextW(dc,L"Запомнить ключ на этом Windows-аккаунте",-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX); SelectObject(dc,old);
            } else if(id==kForget) {
                // Secondary ghost button: same radius/colours as the key input.
                const bool disabled=(d->itemState&ODS_DISABLED)!=0, focus=(d->itemState&ODS_FOCUS)!=0;
                RECT pill=r; if(down) InflateRect(&pill,-1,-1);
                const COLORREF fill=disabled?kBg:(down?RGB(0x18,0x19,0x1f):(hot?kHover:kEditBg));
                const COLORREF edge=disabled?kLine:(focus?kAccent:(hot?kMuted:kInputBorder));
                rounded(dc,pill,12,fill,edge);
                auto old=SelectObject(dc,smallFont); SetTextColor(dc,disabled?kIdle:(hot?kBright:kText));
                DrawTextW(dc,L"Забыть",-1,&pill,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX); SelectObject(dc,old);
            } else if(id==kShow) {
                if(hot) rounded(dc,r,10,kHover,kHover);
                drawEye(dc,r,visible?kAccent:(hot?kText:kMuted),visible);
            } else {
                const COLORREF c=down?(id==kClose?kAccent:kBright):(hot?kBright:kMuted);
                const float cx=(r.left+r.right)/2.f, cy=(r.top+r.bottom)/2.f;
                if(id==kMinimize) line(dc,cx-7.f,cy+1.f,cx+7.f,cy+1.f,c,2.f);
                else { line(dc,cx-7.f,cy-7.f,cx+7.f,cy+7.f,c,2.f); line(dc,cx+7.f,cy-7.f,cx-7.f,cy+7.f,c,2.f); }
            }
            if(oldBmp) { BitBlt(d->hDC,r.left,r.top,r.right-r.left,r.bottom-r.top,mem,r.left,r.top,SRCCOPY); SelectObject(mem,oldBmp); }
            if(bmp)DeleteObject(bmp); if(mem)DeleteDC(mem);
            return TRUE;
        }
        case WM_COMMAND:
            if(LOWORD(w)==kForget && HIWORD(w)==BN_CLICKED && !job) {
                const bool ok=albion::ForgetLicenseCache();
                if(ok) SetWindowTextW(edit,L"");
                setStatus(ok?0:2,ok?L"Сохранённый ключ удалён.":L"Не удалось удалить ключ. Закройте другие экземпляры и повторите."); return 0;
            }
            if(LOWORD(w)==kRemember && HIWORD(w)==BN_CLICKED && !job) {
                rememberKey=!rememberKey; InvalidateRect(remember,nullptr,FALSE);
                if(!rememberKey && !albion::ForgetLicenseCache()) setStatus(2,L"Не удалось удалить сохранённый ключ.");
                return 0;
            }
            if(LOWORD(w)==kSubmit && HIWORD(w)==BN_CLICKED) { startRequest(); return 0; }
            if(LOWORD(w)==kShow && HIWORD(w)==BN_CLICKED) { visible=!visible; SendMessageW(edit,EM_SETPASSWORDCHAR,visible?0:0x2022,0); InvalidateRect(edit,nullptr,TRUE); InvalidateRect(show,nullptr,FALSE); SetFocus(edit); return 0; }
            if(LOWORD(w)==kMinimize && HIWORD(w)==BN_CLICKED) { ShowWindow(h,SW_MINIMIZE); return 0; }
            if(LOWORD(w)==kClose && HIWORD(w)==BN_CLICKED) { SendMessageW(h,WM_CLOSE,0,0); return 0; }
            if(LOWORD(w)==kEdit && HIWORD(w)==EN_CHANGE && statusKind==2) { setStatus(0,L"\u0412\u0432\u0435\u0434\u0438\u0442\u0435 \u043a\u043b\u044e\u0447 \u0434\u043b\u044f \u043f\u0440\u043e\u0432\u0435\u0440\u043a\u0438."); return 0; }
            break;
        case WM_TIMER:
            if(w==kFadeTimer) { // 220 ms fade-in
                fadeStep=std::min(fadeStep+1,11); SetLayeredWindowAttributes(h,0,static_cast<BYTE>(255*fadeStep/11),LWA_ALPHA);
                if(fadeStep>=11) KillTimer(h,kFadeTimer); return 0; }
            if(w==kSuccessTimer) { // short "ready" beat, then freeze into the hand-off snapshot
                KillTimer(h,kSuccessTimer); handoffCreate(instance,h);
                done=true; SetWindowTextW(edit,L""); DestroyWindow(h); return 0; }
            if(w==kDotsTimer) { ++dots; InvalidateRect(submit,nullptr,FALSE); return 0; }
            if(w==kTimer && job) { int result=job->state.load(std::memory_order_acquire); if(result) {
                const bool cacheFailed=job->cacheFailed.load(); job.reset(); if(result==1) {
                if(cacheFailed) MessageBoxW(h,L"Лицензия подтверждена, но сохранение/удаление ключа не удалось. Закройте другие экземпляры и проверьте доступ к папке данных. При следующем входе может потребоваться ключ.",L"Сохранение ключа",MB_ICONWARNING);
                accepted=true; setStatus(3,L"\u041b\u0438\u0446\u0435\u043d\u0437\u0438\u044f \u043f\u043e\u0434\u0442\u0432\u0435\u0440\u0436\u0434\u0435\u043d\u0430. \u0417\u0430\u0433\u0440\u0443\u0437\u043a\u0430..."); InvalidateRect(h,nullptr,FALSE); if (AlbionReducedMotion() || !SetTimer(h,kSuccessTimer,320,nullptr)) {
                    done=true; SetWindowTextW(edit,L""); DestroyWindow(h);
                } return 0; }
                EnableWindow(edit,TRUE); EnableWindow(submit,TRUE); EnableWindow(remember,TRUE); EnableWindow(forget,TRUE); InvalidateRect(h,&inputRc,FALSE);
                setStatus(2,result==2?L"\u041a\u043b\u044e\u0447 \u043d\u0435 \u043f\u0440\u0438\u043d\u044f\u0442. \u041f\u0440\u043e\u0432\u0435\u0440\u044c\u0442\u0435 \u0435\u0433\u043e \u0438 \u043f\u043e\u0432\u0442\u043e\u0440\u0438\u0442\u0435.":L"\u041e\u0448\u0438\u0431\u043a\u0430 \u0441\u0435\u0442\u0438 \u0438\u043b\u0438 \u0438\u043d\u0438\u0446\u0438\u0430\u043b\u0438\u0437\u0430\u0446\u0438\u0438. \u041f\u043e\u043f\u0440\u043e\u0431\u0443\u0439\u0442\u0435 \u0435\u0449\u0451 \u0440\u0430\u0437."); SetFocus(edit);
            } return 0; } break;
        case WM_CLOSE: if(job) job->cancelled=true; done=true; if(edit)SetWindowTextW(edit,L""); DestroyWindow(h); return 0;
        case WM_DESTROY: KillTimer(h,kTimer); KillTimer(h,kFadeTimer); KillTimer(h,kDotsTimer); KillTimer(h,kSuccessTimer); window=nullptr; done=true; return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }
};
LRESULT CALLBACK GateProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    Gate* g=reinterpret_cast<Gate*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE) { g=reinterpret_cast<Gate*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(g)); }
    try { return g?g->proc(h,m,w,l):DefWindowProcW(h,m,w,l); }
    catch (...) {
        MessageBoxW(h,L"License form operation failed. Online verification was not bypassed.",L"Error",MB_ICONERROR);
        if(g) g->done=true;
        return m==WM_CREATE?-1:0;
    }
}
}
bool AlbionAuthHandoffRect(RECT* out) {
    if(!g_handoff.wnd || !out) return false; *out=g_handoff.rc; return true;
}
void AlbionAuthFinishHandoff(HWND settings) {
    if(!g_handoff.wnd || g_handoff.fading) return;
    if(settings) { // owned popup always stays above its owner, even after activation
        SetWindowLongPtrW(g_handoff.wnd,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(settings));
        SetWindowPos(g_handoff.wnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    g_handoff.fading=true; g_handoff.start=GetTickCount64();
    if(!SetTimer(g_handoff.wnd,kHandoffTimer,15,nullptr)) DestroyWindow(g_handoff.wnd);
}
static bool RunGate(HINSTANCE instance);
bool RunAlbionAuthGate(HINSTANCE instance) {
    ULONG_PTR token=0; Gdiplus::GdiplusStartupInput gsi;
    const bool gdip=Gdiplus::GdiplusStartup(&token,&gsi,nullptr)==Gdiplus::Ok;
    const bool ok=RunGate(instance);
    if(gdip) Gdiplus::GdiplusShutdown(token);
    return ok;
}
static bool RunGate(HINSTANCE instance) {
    Gate g; g.instance=instance;
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc=GateProc; wc.hInstance=instance; wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.lpszClassName=L"AlbionLicenseGateV4";
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(IDI_WINDOWSPROJECT1));
    wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(IDI_WINDOWSPROJECT1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED));
    if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    constexpr int width=kWidth,height=kHeight;
    POINT p{}; GetCursorPos(&p); MONITORINFO mi{}; mi.cbSize=sizeof(mi); HMONITOR mon=MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST);
    int x=CW_USEDEFAULT,y=CW_USEDEFAULT; if(mon && GetMonitorInfoW(mon,&mi)) { x=mi.rcWork.left+std::max<LONG>(0,((mi.rcWork.right-mi.rcWork.left)-width)/2); y=mi.rcWork.top+std::max<LONG>(0,((mi.rcWork.bottom-mi.rcWork.top)-height)/2); }
    g.window=CreateWindowExW(WS_EX_LAYERED|WS_EX_APPWINDOW,wc.lpszClassName,L"Albion AI",WS_POPUP|WS_MINIMIZEBOX|WS_SYSMENU|WS_CLIPCHILDREN,x,y,width,height,nullptr,nullptr,instance,&g);
    if(!g.window) return false;
    SetLayeredWindowAttributes(g.window,0,0,LWA_ALPHA);
    HRGN region=CreateRoundRectRgn(0,0,width+1,height+1,kCorner,kCorner); if(region && !SetWindowRgn(g.window,region,FALSE)) DeleteObject(region);
    ShowWindow(g.window,SW_SHOW); UpdateWindow(g.window); SetFocus(g.edit);
    if(AlbionReducedMotion() || !SetTimer(g.window,kFadeTimer,20,nullptr)) SetLayeredWindowAttributes(g.window,0,255,LWA_ALPHA);
    MSG msg{}; while(!g.done) {
        int ret=GetMessageW(&msg,nullptr,0,0); if(ret<=0) { if(ret==0)PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        if(msg.message==WM_KEYDOWN && GetAncestor(msg.hwnd,GA_ROOT)==g.window) {
            // FIX-V11.1-AUTH-KEYS: Enter on the focused "Forget" button must
            // activate that button, not silently start an online login instead.
            if(msg.wParam==VK_RETURN && IsWindowEnabled(g.submit) && msg.hwnd!=g.forget) { g.startRequest(); continue; }
            if(msg.wParam==VK_ESCAPE) { SendMessageW(g.window,WM_CLOSE,0,0); continue; }
        }
        if(!IsDialogMessageW(g.window,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if(g.window) DestroyWindow(g.window); return g.accepted;
}
