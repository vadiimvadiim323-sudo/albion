#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <unknwn.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <string>
#include <functional>
#include <algorithm>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"comctl32.lib")
namespace albion {
// Owned, keyboard-accessible modal. Uses the same palette and borrowed fonts
// as the settings window. It does not start conversion or touch model state.
class ConversionDialog {
public:
    using Created=std::function<void(HWND)>;
    static bool Ask(HWND owner,const std::wstring& source,HFONT body,HFONT title,HFONT captionFont,Created created={}) {
        ConversionDialog d;d.owner=owner;d.source=source;d.body=body;d.title=title;d.captionFont=captionFont;
        WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=&Proc;wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"AlbionConversionConfirmationV2";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.style=CS_DROPSHADOW;
        if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        RECT work{};MONITORINFO mi{sizeof(mi)};
        if(GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTOPRIMARY),&mi))work=mi.rcWork;
        else SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        RECT anchor=work;if(owner&&IsWindow(owner))GetWindowRect(owner,&anchor);
        int x=(anchor.left+anchor.right-W)/2,y=(anchor.top+anchor.bottom-H)/2;
        x=std::clamp<int>(x,int(work.left),int(std::max(work.left,work.right-W)));
        y=std::clamp<int>(y,int(work.top),int(std::max(work.top,work.bottom-H)));
        d.hwnd=CreateWindowExW(WS_EX_CONTROLPARENT|WS_EX_TOOLWINDOW,wc.lpszClassName,L"\u041a\u043e\u043d\u0432\u0435\u0440\u0442\u0430\u0446\u0438\u044f \u043c\u043e\u0434\u0435\u043b\u0438",
            WS_POPUP|WS_SYSMENU|WS_CLIPCHILDREN,x,y,W,H,owner,nullptr,wc.hInstance,&d);
        if(!d.hwnd)return false;
        const bool wasEnabled=owner&&IsWindow(owner)&&IsWindowEnabled(owner);
        if(wasEnabled)EnableWindow(owner,FALSE);
        BOOL dark=TRUE;DwmSetWindowAttribute(d.hwnd,20,&dark,sizeof(dark));
        int corner=2;DwmSetWindowAttribute(d.hwnd,33,&corner,sizeof(corner));
        SetWindowRgn(d.hwnd,CreateRoundRectRgn(0,0,W+1,H+1,20,20),FALSE);
        ShowWindow(d.hwnd,SW_SHOWNORMAL);UpdateWindow(d.hwnd);SetForegroundWindow(d.hwnd);SetFocus(d.cancel);
        bool quit=false;int quitCode=0;
        try {
            if(created)created(d.hwnd);
            MSG msg{};
            while(!d.done&&IsWindow(d.hwnd)) {
                const BOOL got=GetMessageW(&msg,nullptr,0,0);
                if(got<=0){quit=got==0;quitCode=int(msg.wParam);break;}
                const bool ours=msg.hwnd==d.hwnd||IsChild(d.hwnd,msg.hwnd);
                if(ours&&msg.message==WM_KEYDOWN) {
                    if(msg.wParam==VK_ESCAPE){d.Finish(false);continue;}
                    if(msg.wParam==VK_TAB||msg.wParam==VK_LEFT||msg.wParam==VK_RIGHT) {
                        SetFocus(GetFocus()==d.start?d.cancel:d.start);continue;
                    }
                    if(msg.wParam==VK_RETURN) {
                        d.Finish(GetFocus()==d.start);continue;
                    }
                }
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
        }catch(...){d.accepted=false;}
        // Restore owner before destroying the active modal, as DialogBox does.
        if(wasEnabled&&IsWindow(owner))EnableWindow(owner,TRUE);
        if(IsWindow(d.hwnd))DestroyWindow(d.hwnd);
        if(wasEnabled&&IsWindow(owner)){SetActiveWindow(owner);SetForegroundWindow(owner);}
        if(quit)PostQuitMessage(quitCode);
        return d.accepted;
    }
private:
    static constexpr int W=700,H=560;
    static constexpr COLORREF Panel=RGB(17,18,23),Card=RGB(21,22,28),Input=RGB(28,29,35),Border=RGB(48,49,58);
    static constexpr COLORREF White=RGB(255,255,255),Text=RGB(224,224,224),Muted=RGB(160,160,160),Accent=RGB(255,53,72);
    HWND owner=nullptr,hwnd=nullptr,start=nullptr,cancel=nullptr,close=nullptr,hot=nullptr;
    HFONT body=nullptr,title=nullptr,captionFont=nullptr;
    std::wstring source;
    bool done=false,accepted=false;
    void Finish(bool ok){accepted=ok;done=true;}
    static Gdiplus::Color Color(COLORREF c){return {255,GetRValue(c),GetGValue(c),GetBValue(c)};}
    static void Round(HDC dc,RECT r,COLORREF fill,COLORREF stroke,int radius=10) {
        Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath p;const float x=float(r.left)+.5f,y=float(r.top)+.5f,w=float(r.right-r.left)-1,h=float(r.bottom-r.top)-1,d=float(radius*2);
        p.AddArc(x,y,d,d,180.f,90.f);p.AddArc(x+w-d,y,d,d,270.f,90.f);
        p.AddArc(x+w-d,y+h-d,d,d,0.f,90.f);p.AddArc(x,y+h-d,d,d,90.f,90.f);p.CloseFigure();
        Gdiplus::SolidBrush b(Color(fill));Gdiplus::Pen pen(Color(stroke),1.f);g.FillPath(&b,&p);g.DrawPath(&pen,&p);
    }
    static void Label(HDC dc,const wchar_t* text,RECT r,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE) {
        const auto old=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,text,-1,&r,flags);SelectObject(dc,old);
    }
    void Paint(HDC dc) {
        RECT all{0,0,W,H};HBRUSH bg=CreateSolidBrush(Panel);FillRect(dc,&all,bg);DeleteObject(bg);
        Round(dc,{0,0,W,H},Panel,Border,10);
        Round(dc,{24,24,72,72},Input,Input,12);
        {Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Gdiplus::Pen pen(Color(White),1.8f);
         g.DrawRectangle(&pen,37.f,37.f,22.f,22.f);
         for(int i=0;i<3;++i){float o=41.f+float(i)*7.f;g.DrawLine(&pen,o,33.f,o,37.f);g.DrawLine(&pen,o,59.f,o,63.f);g.DrawLine(&pen,33.f,o,37.f,o);g.DrawLine(&pen,59.f,o,63.f,o);}
         g.DrawLine(&pen,44.f,49.f,48.f,43.f);g.DrawLine(&pen,48.f,43.f,48.f,49.f);g.DrawLine(&pen,48.f,49.f,52.f,49.f);g.DrawLine(&pen,52.f,49.f,48.f,55.f);}
        Label(dc,L"\u041a\u043e\u043d\u0432\u0435\u0440\u0442\u0430\u0446\u0438\u044f \u043c\u043e\u0434\u0435\u043b\u0438",{88,19,626,53},title,White);
        Label(dc,L"ONNX \u2192 TensorRT FP16",{90,55,620,77},captionFont,Muted);
        HBRUSH line=CreateSolidBrush(Border);RECT sep{24,92,W-24,93};FillRect(dc,&sep,line);
        RECT footer{24,483,W-24,484};FillRect(dc,&footer,line);DeleteObject(line);
        Round(dc,{24,111,W-24,178},Card,Border,9);
        Label(dc,L"\u0418\u0421\u0425\u041e\u0414\u041d\u0410\u042f \u041c\u041e\u0414\u0415\u041b\u042c",{40,120,W-42,140},captionFont,Muted);
        const auto slash=source.find_last_of(L"\\/");const auto name=slash==std::wstring::npos?source:source.substr(slash+1);
        Label(dc,name.c_str(),{40,142,W-42,166},body,White,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        const wchar_t* chips[]={L"FP16",L"GPU NVIDIA",L"\u041e\u043f\u0442\u0438\u043c\u0438\u0437\u0430\u0446\u0438\u044f 3"};
        for(int i=0;i<3;++i){int x=24+i*222;Round(dc,{x,194,x+208,226},Input,Input,8);Label(dc,chips[i],{x,194,x+208,226},captionFont,Text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        Label(dc,L"\u041f\u0435\u0440\u0435\u0434 \u043d\u0430\u0447\u0430\u043b\u043e\u043c",{28,246,W-28,270},body,White);
        Label(dc,L"YOLOv8 / YOLO11 \u0431\u0435\u0437 \u0432\u0441\u0442\u0440\u043e\u0435\u043d\u043d\u043e\u0433\u043e NMS.",{28,282,W-28,304},body,Text);
        Label(dc,L"FP32: \u0432\u0445\u043e\u0434 [1,3,H,H], H=64\u2013640; \u0432\u044b\u0445\u043e\u0434 [1,C,N].",{28,308,W-28,330},captionFont,Muted);
        Label(dc,L"\u0420\u0430\u0441\u043f\u043e\u0437\u043d\u0430\u0432\u0430\u043d\u0438\u0435 \u0431\u0443\u0434\u0435\u0442 \u043d\u0430 \u043f\u0430\u0443\u0437\u0435 \u0434\u043e \u0437\u0430\u0432\u0435\u0440\u0448\u0435\u043d\u0438\u044f \u0441\u0431\u043e\u0440\u043a\u0438.",{28,344,W-28,366},body,Text);
        Label(dc,L"Engine \u2014 \u0440\u044f\u0434\u043e\u043c \u0441 EXE, \u043a\u043e\u043f\u0438\u044f ONNX \u2014 \u0432 onnx_sources.",{28,374,W-28,396},body,Text);
        Label(dc,L"\u041e\u0440\u0438\u0433\u0438\u043d\u0430\u043b \u0438 \u0441\u0443\u0449\u0435\u0441\u0442\u0432\u0443\u044e\u0449\u0438\u0435 engine \u043d\u0435 \u0443\u0434\u0430\u043b\u044f\u044e\u0442\u0441\u044f.",{28,400,W-28,422},body,Text);
        Label(dc,L"\u0421\u0431\u043e\u0440\u043a\u0430 \u043c\u043e\u0436\u0435\u0442 \u0437\u0430\u043d\u044f\u0442\u044c \u043d\u0435\u0441\u043a\u043e\u043b\u044c\u043a\u043e \u043c\u0438\u043d\u0443\u0442. \u0414\u043e\u0441\u0442\u0443\u043f\u043d\u0430 \u043e\u0442\u043c\u0435\u043d\u0430.",{28,438,W-28,457},captionFont,Muted);
        Label(dc,L"\u041b\u043e\u043a\u0430\u043b\u044c\u043d\u0430\u044f \u043a\u043e\u043d\u0432\u0435\u0440\u0442\u0430\u0446\u0438\u044f",{28,503,330,539},captionFont,Muted);
        Label(dc,L"\u0423\u0441\u043a\u043e\u0440\u0435\u043d\u0438\u0435 \u043e\u0442\u043d\u043e\u0441\u0438\u0442\u0435\u043b\u044c\u043d\u043e \u0438\u0441\u0445\u043e\u0434\u043d\u043e\u0439 \u043c\u043e\u0434\u0435\u043b\u0438 \u043d\u0435 \u0433\u0430\u0440\u0430\u043d\u0442\u0438\u0440\u0443\u0435\u0442\u0441\u044f.",{28,457,W-28,476},captionFont,Muted);
    }
    void DrawButton(DRAWITEMSTRUCT* di) {
        RECT r=di->rcItem;HDC dc=di->hDC;
        HBRUSH bg=CreateSolidBrush(Panel);FillRect(dc,&r,bg);DeleteObject(bg);
        const bool pressed=(di->itemState&ODS_SELECTED)!=0,hover=hot==di->hwndItem,focus=(di->itemState&ODS_FOCUS)!=0;
        if(di->hwndItem==close) {
            if(hover||pressed)Round(dc,r,Input,Input,7);
            Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);Gdiplus::Pen p(Color(hover?White:Muted),1.6f);
            const float x=float(r.right)/2,y=float(r.bottom)/2;g.DrawLine(&p,x-5,y-5,x+5,y+5);g.DrawLine(&p,x+5,y-5,x-5,y+5);return;
        }
        const bool primary=di->hwndItem==start;
        const COLORREF fill=primary?(pressed?RGB(189,38,38):hover?RGB(235,43,61):Accent):(hover||pressed?RGB(45,47,56):Input);
        Round(dc,r,fill,focus?White:(primary?fill:Border),9);
        Label(dc,primary?L"\u041a\u043e\u043d\u0432\u0435\u0440\u0442\u0438\u0440\u043e\u0432\u0430\u0442\u044c":L"\u041e\u0442\u043c\u0435\u043d\u0430",r,body,primary?White:Text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    static LRESULT CALLBACK ButtonProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR ref) {
        auto* d=reinterpret_cast<ConversionDialog*>(ref);
        if(m==WM_MOUSEMOVE&&d->hot!=h){if(d->hot)InvalidateRect(d->hot,nullptr,FALSE);d->hot=h;InvalidateRect(h,nullptr,FALSE);TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}
        if(m==WM_MOUSELEAVE){if(d->hot==h)d->hot=nullptr;InvalidateRect(h,nullptr,FALSE);}
        if(m==WM_SETFOCUS||m==WM_KILLFOCUS)InvalidateRect(h,nullptr,FALSE);
        if(m==WM_NCDESTROY)RemoveWindowSubclass(h,&ButtonProc,id);
        return DefSubclassProc(h,m,w,l);
    }
    void Create() {
        auto button=[this](const wchar_t* text,int id,int x,int y,int w,int h){HWND b=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,x,y,w,h,hwnd,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);if(b){SendMessageW(b,WM_SETFONT,reinterpret_cast<WPARAM>(body),FALSE);SetWindowSubclass(b,&ButtonProc,1,reinterpret_cast<DWORD_PTR>(this));}return b;};
        close=button(L"\u0417\u0430\u043a\u0440\u044b\u0442\u044c",IDCLOSE,W-52,24,28,28);
        cancel=button(L"\u041e\u0442\u043c\u0435\u043d\u0430",IDCANCEL,W-354,503,144,36);
        start=button(L"\u041a\u043e\u043d\u0432\u0435\u0440\u0442\u0438\u0440\u043e\u0432\u0430\u0442\u044c",IDOK,W-198,503,174,36);
    }
    static LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
        ConversionDialog* d=reinterpret_cast<ConversionDialog*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE){d=static_cast<ConversionDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);d->hwnd=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(d));}
        if(!d)return DefWindowProcW(h,m,w,l);
        switch(m) {
        case WM_CREATE:d->Create();if(!d->start||!d->cancel||!d->close)return -1;return 0;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,W,H);if(mem&&bmp){const auto old=SelectObject(mem,bmp);d->Paint(mem);BitBlt(dc,0,0,W,H,mem,0,0,SRCCOPY);SelectObject(mem,old);}else d->Paint(dc);if(bmp)DeleteObject(bmp);if(mem)DeleteDC(mem);EndPaint(h,&ps);return 0;}
        case WM_PRINTCLIENT:d->Paint(reinterpret_cast<HDC>(w));return 0;
        case WM_DRAWITEM:if(l)d->DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
        case WM_COMMAND:if(HIWORD(w)==BN_CLICKED){if(LOWORD(w)==IDOK)d->Finish(true);else if(LOWORD(w)==IDCANCEL||LOWORD(w)==IDCLOSE)d->Finish(false);}return 0;
        case WM_NCHITTEST:{POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&p);if(p.y<90&&p.x<W-62)return HTCAPTION;return HTCLIENT;}
        case WM_SYSCOMMAND:if((w&0xfff0)==SC_KEYMENU)return 0;break;
        case WM_CLOSE:d->Finish(false);return 0;
        case WM_DESTROY:d->done=true;return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
        }
        return DefWindowProcW(h,m,w,l);
    }
};
} // namespace albion
