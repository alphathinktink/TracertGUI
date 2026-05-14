//------------------------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#elif _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
//------------------------------------------------------------------------------------------
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
//------------------------------------------------------------------------------------------
#define IDC_HOST        1001
#define IDC_FAMILY      1002
#define IDC_START       1003
#define IDC_AUTO        1004
#define IDC_INTERVAL    1005
#define IDC_STATUS      1006
#define IDC_RESULTS     1007

#define WM_APP_TRACE_UPDATE     (WM_APP + 1)
#define WM_APP_TRACE_DONE       (WM_APP + 2)
#define WM_APP_NAME_UPDATE      (WM_APP + 3)

#define TRACE_MAX_HOPS          30
#define TRACE_PROBES            3
#define TRACE_TIMEOUT_MS        1500
#define TRACE_REFRESH_TIMER     42
#define TRACE_DEFAULT_REFRESH   60
//------------------------------------------------------------------------------------------
struct HopProbe
{
    bool replied = false;
    DWORD status = IP_REQ_TIMED_OUT;
    DWORD ms = 0;
};

struct HopRow
{
    int hop = 0;
    bool complete = false;
    bool destination = false;
    bool resolving = false;
    WCHAR address[NI_MAXHOST] = L"";
    WCHAR name[NI_MAXHOST] = L"";
    WCHAR status[64] = L"Pending";
    HopProbe probes[TRACE_PROBES];
};

struct TraceConfig
{
    WCHAR host[256] = L"";
    int family = AF_INET;
    bool autoRefresh = false;
    DWORD intervalSeconds = TRACE_DEFAULT_REFRESH;
};

struct AppState
{
    HINSTANCE instance = NULL;
    HWND mainWnd = NULL;
    HWND hostEdit = NULL;
    HWND familyCombo = NULL;
    HWND startButton = NULL;
    HWND autoCheck = NULL;
    HWND intervalEdit = NULL;
    HWND statusText = NULL;
    HWND resultsView = NULL;
    HFONT uiFont = NULL;
    CRITICAL_SECTION lock;
    std::vector<HopRow> rows;
    std::vector<HANDLE> lookupThreads;
    HANDLE traceThread = NULL;
    volatile LONG cancelTrace = 0;
    bool running = false;
};

struct TraceThreadContext
{
    AppState* app = NULL;
    TraceConfig config;
};

struct NameLookupContext
{
    AppState* app = NULL;
    int hop = 0;
    int family = AF_UNSPEC;
    sockaddr_storage addr = {};
    int addrLen = 0;
};
//------------------------------------------------------------------------------------------
static const WCHAR* MainClassName = L"TracertGuiMainWindow";
static const WCHAR* ResultsClassName = L"TracertGuiResultsView";
static const WCHAR* AppCredit = L"Utility written by GPT-5.5, a model created by OpenAI.";
//------------------------------------------------------------------------------------------
static void SetWindowTextFormat(HWND wnd, const WCHAR* fmt, ...)
{
    WCHAR buffer[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buffer, ARRAYSIZE(buffer), fmt, args);
    buffer[ARRAYSIZE(buffer) - 1] = 0;
    va_end(args);
    SetWindowTextW(wnd, buffer);
}

static std::wstring FormatWin32Error(DWORD code)
{
    WCHAR* message = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, (LPWSTR)&message, 0, NULL);
    std::wstring result = message ? message : L"Unknown error";
    if (message)
        LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L'.'))
        result.pop_back();
    return result;
}

static const WCHAR* IcmpStatusText(DWORD status)
{
    switch (status)
    {
    case IP_SUCCESS: return L"Reply";
    case IP_BUF_TOO_SMALL: return L"Buffer too small";
    case IP_DEST_NET_UNREACHABLE: return L"Network unreachable";
    case IP_DEST_HOST_UNREACHABLE: return L"Host unreachable";
    case IP_DEST_PROT_UNREACHABLE: return L"Protocol unreachable";
    case IP_DEST_PORT_UNREACHABLE: return L"Port unreachable";
    case IP_REQ_TIMED_OUT: return L"Timed out";
    case IP_BAD_REQ: return L"Bad request";
    case IP_BAD_ROUTE: return L"Bad route";
    case IP_TTL_EXPIRED_TRANSIT: return L"TTL expired";
    case IP_TTL_EXPIRED_REASSEM: return L"TTL expired";
    case IP_PARAM_PROBLEM: return L"Parameter problem";
    case IP_SOURCE_QUENCH: return L"Source quench";
    case IP_OPTION_TOO_BIG: return L"Option too big";
    case IP_BAD_DESTINATION: return L"Bad destination";
    case IP_GENERAL_FAILURE: return L"General failure";
    default: return L"ICMP status";
    }
}

static void CopyText(WCHAR* dest, size_t destChars, const WCHAR* src)
{
    if (!dest || destChars == 0)
        return;
    if (!src)
        src = L"";
    wcsncpy(dest, src, destChars - 1);
    dest[destChars - 1] = 0;
}

static int GetIntervalSeconds(HWND edit)
{
    WCHAR text[32];
    GetWindowTextW(edit, text, ARRAYSIZE(text));
    int value = _wtoi(text);
    if (value < 5)
        value = 5;
    if (value > 3600)
        value = 3600;
    return value;
}

static bool IsTerminalIcmpStatus(DWORD status)
{
    return status == IP_SUCCESS || status == IP_DEST_HOST_UNREACHABLE || status == IP_DEST_NET_UNREACHABLE;
}

static void BeginRows(AppState* app)
{
    EnterCriticalSection(&app->lock);
    app->rows.clear();
    app->rows.reserve(TRACE_MAX_HOPS);
    LeaveCriticalSection(&app->lock);
    InvalidateRect(app->resultsView, NULL, TRUE);
}

static void UpdateHop(AppState* app, const HopRow& row)
{
    EnterCriticalSection(&app->lock);
    if ((int)app->rows.size() < row.hop)
        app->rows.resize(row.hop);
    app->rows[row.hop - 1] = row;
    LeaveCriticalSection(&app->lock);
    PostMessageW(app->mainWnd, WM_APP_TRACE_UPDATE, 0, 0);
}

static void StartNameLookup(AppState* app, int hop, const sockaddr_storage& addr, int addrLen, int family)
{
    NameLookupContext* ctx = new NameLookupContext;
    ctx->app = app;
    ctx->hop = hop;
    ctx->family = family;
    ctx->addr = addr;
    ctx->addrLen = addrLen;
    HANDLE thread = CreateThread(NULL, 0, [](LPVOID param) -> DWORD
    {
        NameLookupContext* lookup = (NameLookupContext*)param;
        WCHAR host[NI_MAXHOST] = L"";
        DWORD flags = NI_NAMEREQD;
        int rc = GetNameInfoW((sockaddr*)&lookup->addr, lookup->addrLen, host, ARRAYSIZE(host), NULL, 0, flags);
        if (rc != 0)
            host[0] = 0;

        EnterCriticalSection(&lookup->app->lock);
        if (lookup->hop > 0 && lookup->hop <= (int)lookup->app->rows.size())
        {
            HopRow& row = lookup->app->rows[lookup->hop - 1];
            row.resolving = false;
            if (host[0])
                CopyText(row.name, ARRAYSIZE(row.name), host);
            else if (!row.name[0])
                CopyText(row.name, ARRAYSIZE(row.name), L"(no reverse name)");
        }
        LeaveCriticalSection(&lookup->app->lock);
        PostMessageW(lookup->app->mainWnd, WM_APP_NAME_UPDATE, 0, 0);
        delete lookup;
        return 0;
    }, ctx, 0, NULL);

    if (thread)
    {
        EnterCriticalSection(&app->lock);
        app->lookupThreads.push_back(thread);
        LeaveCriticalSection(&app->lock);
    }
    else
        delete ctx;
}

static bool ResolveTarget(const TraceConfig& config, sockaddr_storage* outAddr, int* outLen, WCHAR* display, size_t displayChars, std::wstring* error)
{
    ADDRINFOW hints = {};
    hints.ai_family = config.family;
    hints.ai_socktype = 0;
    hints.ai_protocol = 0;

    ADDRINFOW* result = NULL;
    int rc = GetAddrInfoW(config.host, NULL, &hints, &result);
    if (rc != 0 || !result)
    {
        WCHAR message[128];
        wsprintfW(message, L"Unable to resolve host (GetAddrInfoW error %d).", rc);
        *error = message;
        return false;
    }

    memcpy(outAddr, result->ai_addr, result->ai_addrlen);
    *outLen = (int)result->ai_addrlen;
    GetNameInfoW((sockaddr*)outAddr, *outLen, display, (DWORD)displayChars, NULL, 0, NI_NUMERICHOST);
    FreeAddrInfoW(result);
    return true;
}

static void ProbeIpv4(HANDLE icmp, const sockaddr_in& target, int ttl, HopRow* row, sockaddr_storage* replyAddr, int* replyAddrLen)
{
    char requestData[] = "TracertGUI";
    DWORD replySize = sizeof(ICMP_ECHO_REPLY) + sizeof(requestData) + 64;
    std::vector<BYTE> reply(replySize);
    IP_OPTION_INFORMATION options = {};
    options.Ttl = (UCHAR)ttl;

    for (int i = 0; i < TRACE_PROBES; ++i)
    {
        DWORD sent = IcmpSendEcho(icmp, target.sin_addr.S_un.S_addr, requestData, sizeof(requestData),
                                  &options, reply.data(), replySize, TRACE_TIMEOUT_MS);
        if (sent > 0)
        {
            ICMP_ECHO_REPLY* echo = (ICMP_ECHO_REPLY*)reply.data();
            row->probes[i].replied = echo->Status != IP_REQ_TIMED_OUT;
            row->probes[i].status = echo->Status;
            row->probes[i].ms = echo->RoundTripTime;
            if (echo->Address && !row->address[0])
            {
                sockaddr_in from = {};
                from.sin_family = AF_INET;
                from.sin_addr.S_un.S_addr = echo->Address;
                GetNameInfoW((sockaddr*)&from, sizeof(from), row->address, ARRAYSIZE(row->address), NULL, 0, NI_NUMERICHOST);
                memcpy(replyAddr, &from, sizeof(from));
                *replyAddrLen = sizeof(from);
            }
        }
        else
        {
            row->probes[i].status = IP_REQ_TIMED_OUT;
        }
    }
}

static void ProbeIpv6(HANDLE icmp, const sockaddr_in6& target, int ttl, HopRow* row, sockaddr_storage* replyAddr, int* replyAddrLen)
{
    char requestData[] = "TracertGUI";
    DWORD replySize = sizeof(ICMPV6_ECHO_REPLY) + sizeof(requestData) + 64;
    std::vector<BYTE> reply(replySize);
    IP_OPTION_INFORMATION options = {};
    options.Ttl = (UCHAR)ttl;
    sockaddr_in6 source = {};
    source.sin6_family = AF_INET6;

    for (int i = 0; i < TRACE_PROBES; ++i)
    {
        DWORD sent = Icmp6SendEcho2(icmp, NULL, NULL, NULL, &source, (sockaddr_in6*)&target, requestData,
                                    sizeof(requestData), &options, reply.data(), replySize, TRACE_TIMEOUT_MS);
        if (sent > 0)
        {
            ICMPV6_ECHO_REPLY* echo = (ICMPV6_ECHO_REPLY*)reply.data();
            row->probes[i].replied = echo->Status != IP_REQ_TIMED_OUT;
            row->probes[i].status = echo->Status;
            row->probes[i].ms = echo->RoundTripTime;
            if (!row->address[0])
            {
                sockaddr_in6 from = {};
                from.sin6_family = AF_INET6;
                memcpy(&from.sin6_addr, &echo->Address.sin6_addr, sizeof(from.sin6_addr));
                from.sin6_scope_id = echo->Address.sin6_scope_id;
                GetNameInfoW((sockaddr*)&from, sizeof(from), row->address, ARRAYSIZE(row->address), NULL, 0, NI_NUMERICHOST);
                memcpy(replyAddr, &from, sizeof(from));
                *replyAddrLen = sizeof(from);
            }
        }
        else
        {
            row->probes[i].status = IP_REQ_TIMED_OUT;
        }
    }
}

static void SummarizeHop(HopRow* row)
{
    DWORD bestStatus = IP_REQ_TIMED_OUT;
    for (int i = 0; i < TRACE_PROBES; ++i)
    {
        if (row->probes[i].status == IP_SUCCESS)
        {
            bestStatus = IP_SUCCESS;
            break;
        }
        if (row->probes[i].status != IP_REQ_TIMED_OUT)
            bestStatus = row->probes[i].status;
    }
    row->destination = bestStatus == IP_SUCCESS;
    CopyText(row->status, ARRAYSIZE(row->status), IcmpStatusText(bestStatus));
    row->complete = true;
    if (!row->address[0])
        CopyText(row->address, ARRAYSIZE(row->address), L"*");
}

static DWORD WINAPI TraceThreadProc(LPVOID param)
{
    TraceThreadContext* ctx = (TraceThreadContext*)param;
    AppState* app = ctx->app;
    TraceConfig config = ctx->config;
    delete ctx;

    sockaddr_storage target = {};
    int targetLen = 0;
    WCHAR targetDisplay[NI_MAXHOST] = L"";
    std::wstring error;
    if (!ResolveTarget(config, &target, &targetLen, targetDisplay, ARRAYSIZE(targetDisplay), &error))
    {
        SetWindowTextW(app->statusText, error.c_str());
        PostMessageW(app->mainWnd, WM_APP_TRACE_DONE, 0, 0);
        return 0;
    }

    SetWindowTextFormat(app->statusText, L"Tracing %s (%s)...", config.host, targetDisplay);

    HANDLE icmp = (config.family == AF_INET6) ? Icmp6CreateFile() : IcmpCreateFile();
    if (icmp == INVALID_HANDLE_VALUE)
    {
        std::wstring msg = L"Unable to open ICMP handle: " + FormatWin32Error(GetLastError());
        SetWindowTextW(app->statusText, msg.c_str());
        PostMessageW(app->mainWnd, WM_APP_TRACE_DONE, 0, 0);
        return 0;
    }

    for (int ttl = 1; ttl <= TRACE_MAX_HOPS && InterlockedCompareExchange(&app->cancelTrace, 0, 0) == 0; ++ttl)
    {
        HopRow row = {};
        row.hop = ttl;
        CopyText(row.status, ARRAYSIZE(row.status), L"Probing...");
        sockaddr_storage replyAddr = {};
        int replyAddrLen = 0;

        if (config.family == AF_INET6)
            ProbeIpv6(icmp, *(sockaddr_in6*)&target, ttl, &row, &replyAddr, &replyAddrLen);
        else
            ProbeIpv4(icmp, *(sockaddr_in*)&target, ttl, &row, &replyAddr, &replyAddrLen);

        SummarizeHop(&row);
        if (replyAddrLen > 0)
        {
            row.resolving = true;
            CopyText(row.name, ARRAYSIZE(row.name), L"Resolving...");
        }
        UpdateHop(app, row);
        if (replyAddrLen > 0)
            StartNameLookup(app, ttl, replyAddr, replyAddrLen, config.family);
        if (row.destination || IsTerminalIcmpStatus(row.probes[0].status))
            break;
    }

    IcmpCloseHandle(icmp);
    SetWindowTextW(app->statusText, InterlockedCompareExchange(&app->cancelTrace, 0, 0) ? L"Trace cancelled." : L"Trace complete.");
    PostMessageW(app->mainWnd, WM_APP_TRACE_DONE, 0, 0);
    return 0;
}

static bool ReadTraceConfig(AppState* app, TraceConfig* config)
{
    GetWindowTextW(app->hostEdit, config->host, ARRAYSIZE(config->host));
    if (!config->host[0])
    {
        MessageBoxW(app->mainWnd, L"Enter a host name or IP address to trace.", L"Trace Route", MB_ICONINFORMATION);
        return false;
    }
    int familyIndex = (int)SendMessageW(app->familyCombo, CB_GETCURSEL, 0, 0);
    config->family = familyIndex == 1 ? AF_INET6 : AF_INET;
    config->autoRefresh = SendMessageW(app->autoCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config->intervalSeconds = GetIntervalSeconds(app->intervalEdit);
    return true;
}

static void SetRunning(AppState* app, bool running)
{
    app->running = running;
    SetWindowTextW(app->startButton, running ? L"Stop" : L"Trace");
    EnableWindow(app->hostEdit, !running);
    EnableWindow(app->familyCombo, !running);
}

static void StartTrace(AppState* app)
{
    if (app->running)
    {
        InterlockedExchange(&app->cancelTrace, 1);
        SetWindowTextW(app->statusText, L"Stopping trace...");
        return;
    }

    TraceConfig config;
    if (!ReadTraceConfig(app, &config))
        return;

    if (app->traceThread)
    {
        CloseHandle(app->traceThread);
        app->traceThread = NULL;
    }

    InterlockedExchange(&app->cancelTrace, 0);
    BeginRows(app);
    SetRunning(app, true);

    TraceThreadContext* ctx = new TraceThreadContext;
    ctx->app = app;
    ctx->config = config;
    app->traceThread = CreateThread(NULL, 0, TraceThreadProc, ctx, 0, NULL);
    if (!app->traceThread)
    {
        delete ctx;
        SetRunning(app, false);
        SetWindowTextFormat(app->statusText, L"Unable to start trace thread: %s", FormatWin32Error(GetLastError()).c_str());
    }
}

static void UpdateAutoRefresh(AppState* app)
{
    if (SendMessageW(app->autoCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)
        SetTimer(app->mainWnd, TRACE_REFRESH_TIMER, GetIntervalSeconds(app->intervalEdit) * 1000, NULL);
    else
        KillTimer(app->mainWnd, TRACE_REFRESH_TIMER);
}

static void LayoutControls(AppState* app, int width, int height)
{
    const int margin = 10;
    const int rowH = 24;
    const int gap = 6;
    int x = margin;
    int y = margin;
    int buttonW = 82;
    int comboW = 80;
    int autoW = 98;
    int intervalW = 54;
    int hostW = std::max(140, width - margin * 2 - buttonW - comboW - autoW - intervalW - gap * 6 - 70);

    MoveWindow(app->hostEdit, x, y, hostW, rowH, TRUE); x += hostW + gap;
    MoveWindow(app->familyCombo, x, y, comboW, 120, TRUE); x += comboW + gap;
    MoveWindow(app->startButton, x, y, buttonW, rowH, TRUE); x += buttonW + gap;
    MoveWindow(app->autoCheck, x, y + 2, autoW, rowH, TRUE); x += autoW + gap;
    MoveWindow(app->intervalEdit, x, y, intervalW, rowH, TRUE); x += intervalW + 2;
    MoveWindow(GetDlgItem(app->mainWnd, IDC_STATUS + 10), x, y + 4, 70, rowH, TRUE);

    y += rowH + gap;
    MoveWindow(app->statusText, margin, y, width - margin * 2, rowH, TRUE);
    y += rowH + gap;
    MoveWindow(app->resultsView, margin, y, width - margin * 2, std::max(80, height - y - margin), TRUE);
}

static void DrawTextClipped(HDC dc, const WCHAR* text, RECT rc, UINT format)
{
    DrawTextW(dc, text, -1, &rc, format | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void PaintResults(HWND hwnd, AppState* app)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    HBRUSH bg = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    HFONT oldFont = (HFONT)SelectObject(dc, app->uiFont);
    SetBkMode(dc, TRANSPARENT);

    RECT header = {0, 0, client.right, 28};
    HBRUSH headerBrush = CreateSolidBrush(RGB(232, 238, 247));
    FillRect(dc, &header, headerBrush);
    DeleteObject(headerBrush);
    SetTextColor(dc, RGB(30, 30, 30));

    int cols[] = {0, 48, 230, 390, 520, client.right};
    const WCHAR* titles[] = {L"Hop", L"Address", L"Name", L"RTT", L"Status"};
    for (int i = 0; i < 5; ++i)
    {
        RECT rc = {cols[i] + 8, 6, cols[i + 1] - 4, 26};
        DrawTextClipped(dc, titles[i], rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(224, 224, 224));
    HPEN oldPen = (HPEN)SelectObject(dc, gridPen);
    MoveToEx(dc, 0, 28, NULL);
    LineTo(dc, client.right, 28);

    std::vector<HopRow> rows;
    EnterCriticalSection(&app->lock);
    rows = app->rows;
    LeaveCriticalSection(&app->lock);

    if (rows.empty())
    {
        RECT rc = {12, 44, client.right - 12, client.bottom - 12};
        SetTextColor(dc, RGB(100, 100, 100));
        DrawTextClipped(dc, L"Enter a host and click Trace. Results refresh here as each hop completes.", rc, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    int y = 29;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const HopRow& row = rows[i];
        RECT rowRc = {0, y, client.right, y + 26};
        if (i % 2)
        {
            HBRUSH alt = CreateSolidBrush(RGB(246, 248, 251));
            FillRect(dc, &rowRc, alt);
            DeleteObject(alt);
        }
        if (row.destination)
        {
            HBRUSH done = CreateSolidBrush(RGB(230, 247, 230));
            FillRect(dc, &rowRc, done);
            DeleteObject(done);
        }

        WCHAR hopText[16];
        wsprintfW(hopText, L"%d", row.hop);
        WCHAR rttText[96] = L"";
        for (int p = 0; p < TRACE_PROBES; ++p)
        {
            WCHAR part[24];
            if (row.probes[p].status == IP_REQ_TIMED_OUT)
                CopyText(part, ARRAYSIZE(part), L"* ");
            else
                wsprintfW(part, L"%lums ", row.probes[p].ms);
            wcsncat(rttText, part, ARRAYSIZE(rttText) - wcslen(rttText) - 1);
        }

        const WCHAR* values[] = {hopText, row.address, row.name, rttText, row.status};
        SetTextColor(dc, row.destination ? RGB(0, 96, 0) : RGB(35, 35, 35));
        for (int c = 0; c < 5; ++c)
        {
            RECT rc = {cols[c] + 8, y + 4, cols[c + 1] - 4, y + 24};
            DrawTextClipped(dc, values[c], rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        MoveToEx(dc, 0, y + 26, NULL);
        LineTo(dc, client.right, y + 26);
        y += 26;
        if (y > client.bottom)
            break;
    }

    for (int c = 1; c < 5; ++c)
    {
        MoveToEx(dc, cols[c], 0, NULL);
        LineTo(dc, cols[c], client.bottom);
    }

    SelectObject(dc, oldPen);
    DeleteObject(gridPen);
    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK ResultsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AppState* app = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lParam)->lpCreateParams);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintResults(hwnd, app);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AppState* app = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_CREATE:
    {
        app = (AppState*)((CREATESTRUCTW*)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        app->mainWnd = hwnd;
        app->uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        app->hostEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"8.8.8.8", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        0, 0, 0, 0, hwnd, (HMENU)IDC_HOST, app->instance, NULL);
        app->familyCombo = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                           0, 0, 0, 0, hwnd, (HMENU)IDC_FAMILY, app->instance, NULL);
        SendMessageW(app->familyCombo, CB_ADDSTRING, 0, (LPARAM)L"IPv4");
        SendMessageW(app->familyCombo, CB_ADDSTRING, 0, (LPARAM)L"IPv6");
        SendMessageW(app->familyCombo, CB_SETCURSEL, 0, 0);
        app->startButton = CreateWindowExW(0, L"BUTTON", L"Trace", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                           0, 0, 0, 0, hwnd, (HMENU)IDC_START, app->instance, NULL);
        app->autoCheck = CreateWindowExW(0, L"BUTTON", L"Auto refresh", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         0, 0, 0, 0, hwnd, (HMENU)IDC_AUTO, app->instance, NULL);
        app->intervalEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"60", WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                            0, 0, 0, 0, hwnd, (HMENU)IDC_INTERVAL, app->instance, NULL);
        CreateWindowExW(0, L"STATIC", L"seconds", WS_CHILD | WS_VISIBLE,
                        0, 0, 0, 0, hwnd, (HMENU)(IDC_STATUS + 10), app->instance, NULL);
        app->statusText = CreateWindowExW(0, L"STATIC", AppCredit, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                          0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, app->instance, NULL);
        app->resultsView = CreateWindowExW(WS_EX_CLIENTEDGE, ResultsClassName, NULL, WS_CHILD | WS_VISIBLE,
                                           0, 0, 0, 0, hwnd, (HMENU)IDC_RESULTS, app->instance, app);

        HWND controls[] = {app->hostEdit, app->familyCombo, app->startButton, app->autoCheck, app->intervalEdit, app->statusText, app->resultsView, GetDlgItem(hwnd, IDC_STATUS + 10)};
        for (HWND control : controls)
            SendMessageW(control, WM_SETFONT, (WPARAM)app->uiFont, TRUE);
        return 0;
    }
    case WM_SIZE:
        if (app)
            LayoutControls(app, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_START && HIWORD(wParam) == BN_CLICKED)
            StartTrace(app);
        else if ((LOWORD(wParam) == IDC_AUTO && HIWORD(wParam) == BN_CLICKED) || LOWORD(wParam) == IDC_INTERVAL)
            UpdateAutoRefresh(app);
        return 0;
    case WM_TIMER:
        if (wParam == TRACE_REFRESH_TIMER && app && !app->running)
            StartTrace(app);
        return 0;
    case WM_APP_TRACE_UPDATE:
    case WM_APP_NAME_UPDATE:
        InvalidateRect(app->resultsView, NULL, FALSE);
        return 0;
    case WM_APP_TRACE_DONE:
        if (app->traceThread)
        {
            CloseHandle(app->traceThread);
            app->traceThread = NULL;
        }
        SetRunning(app, false);
        UpdateAutoRefresh(app);
        return 0;
    case WM_CLOSE:
        InterlockedExchange(&app->cancelTrace, 1);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TRACE_REFRESH_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool RegisterWindowClasses(HINSTANCE instance)
{
    WNDCLASSEXW mainClass = {};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.lpfnWndProc = MainWndProc;
    mainClass.hInstance = instance;
    mainClass.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    mainClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    mainClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    mainClass.lpszClassName = MainClassName;
    if (!RegisterClassExW(&mainClass))
        return false;

    WNDCLASSEXW resultsClass = {};
    resultsClass.cbSize = sizeof(resultsClass);
    resultsClass.lpfnWndProc = ResultsWndProc;
    resultsClass.hInstance = instance;
    resultsClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    resultsClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    resultsClass.lpszClassName = ResultsClassName;
    return RegisterClassExW(&resultsClass) != 0;
}
//------------------------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;

    AppState app;
    app.instance = hInstance;
    InitializeCriticalSection(&app.lock);

    if (!RegisterWindowClasses(hInstance))
    {
        DeleteCriticalSection(&app.lock);
        WSACleanup();
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, MainClassName, L"Trace Route GUI - GPT-5.5", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 860, 540,
                                NULL, NULL, hInstance, &app);
    if (!hwnd)
    {
        DeleteCriticalSection(&app.lock);
        WSACleanup();
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    InterlockedExchange(&app.cancelTrace, 1);
    if (app.traceThread)
    {
        WaitForSingleObject(app.traceThread, 3000);
        CloseHandle(app.traceThread);
    }
    EnterCriticalSection(&app.lock);
    std::vector<HANDLE> lookupThreads = app.lookupThreads;
    app.lookupThreads.clear();
    LeaveCriticalSection(&app.lock);
    for (HANDLE thread : lookupThreads)
    {
        WaitForSingleObject(thread, 3000);
        CloseHandle(thread);
    }
    DeleteCriticalSection(&app.lock);
    WSACleanup();
    return (int)msg.wParam;
}
//------------------------------------------------------------------------------------------
