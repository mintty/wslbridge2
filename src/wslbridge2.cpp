/* 
 * This file is part of wslbridge2 project.
 * Licensed under the terms of the GNU General Public License v3 or later.
 * Copyright (C) Biswapriyo Nath.
 */

#include <windows.h>
#include <assert.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#ifdef __CYGWIN__
#include <sys/cygwin.h>
#endif
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "Helpers.hpp"
#include "Environment.hpp"
#include "LocalSock.hpp"
#include "TerminalState.hpp"
#include "WinHelper.hpp"

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

/* Enable this to show debug information */
static const char IsDebugMode = 0;

union IoSockets
{
    int sock[3];
    struct
    {
        int inputSock;
        int outputSock;
        int controlSock;
    };
};

/* global variable */
static union IoSockets g_ioSockets = { 0 };
static class LocalSock *g_locSock = NULL;

static void* resize_window(void *set)
{
    int ret, signum;
    struct winsize winp;

    while (1)
    {
        /* Wait for the window resize signal aka. SIGWINCH */
        ret = sigwait((sigset_t *)set, &signum);
        if (ret != 0 || signum != SIGWINCH)
            break;

        /* Send terminal window size to control socket */
        ioctl(STDIN_FILENO, TIOCGWINSZ, &winp);
        g_locSock->Send(g_ioSockets.controlSock, &winp, sizeof winp);

        if (IsDebugMode)
            printf("cols: %d row: %d\n", winp.ws_col,winp.ws_row);
    }

    pthread_exit(&ret);
    return nullptr;
}

static void* send_buffer(void *param)
{
    int ret;
    char data[1024];

    struct pollfd fds = { STDIN_FILENO, POLLIN, 0 };

    while (1)
    {
        ret = poll(&fds, 1, -1);

        if (fds.revents & POLLIN)
        {
            ret = read(STDIN_FILENO, data, sizeof data);
            if (ret > 0)
                ret = g_locSock->Send(g_ioSockets.inputSock, data, ret);
            else
                break;
        }
    }

    pthread_exit(&ret);
    return nullptr;
}

static void* receive_buffer(void *param)
{
    int ret;
    char data[1024];

    while (1)
    {
        ret = g_locSock->Receive(g_ioSockets.outputSock, data, sizeof data);
        if (ret > 0)
            ret = write(STDOUT_FILENO, data, ret);
        else
            break;
    }

    pthread_exit(&ret);
    return nullptr;
}

struct PipeHandles {
    HANDLE rh;
    HANDLE wh;
};

static struct PipeHandles createPipe()
{
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    PipeHandles ret {};
    const BOOL success = CreatePipe(&ret.rh, &ret.wh, &sa, 0);
    assert(success && "CreatePipe failed");
    return ret;
}

static void usage(const char *prog)
{
    printf(
    "Usage: %s [options] [--] [command]...\n"
    "Runs a program within a Windows Subsystem for Linux (WSL) pty\n"
    "\n"
    "Options:\n"
    "  -b, --backend BACKEND\n"
    "                Overrides the default path of wslbridge2-backend to BACKEND\n"
    "  -d, --distribution Distribution Name\n"
    "                Run the specified distribution.\n"
    "  -e VAR        Copies VAR into the WSL environment.\n"
    "  -e VAR=VAL    Sets VAR to VAL in the WSL environment.\n"
    "  -h, --help    Show this usage information\n"
    "  -l, --login   Start a login shell.\n"
    "  -u, --user    WSL User Name\n"
    "                Run as the specified user\n"
    "  -w, --windir  Folder\n"
    "                Changes the working directory to Windows style path\n"
    "  -W, --wsldir  Folder\n"
    "                Changes the working directory to Unix style path\n",
    prog);
    exit(0);
}

static void invalid_arg(const char *arg)
{
    fatal("error: the %s option requires a non-empty string argument\n", arg);
}

int main(int argc, char *argv[])
{
    if (GetWindowsBuild() < 17763)
        fatal("Windows 10 version is older than minimal requirement.\n");

#ifdef __CYGWIN__
    cygwin_internal(CW_SYNC_WINENV);
#endif

    const char shortopts[] = "+b:d:e:hlu:w:W:";
    const struct option longopts[] = {
        { "backend",       required_argument, 0, 'b' },
        { "distribution",  required_argument, 0, 'd' },
        { "env",           required_argument, 0, 'e' },
        { "help",          no_argument,       0, 'h' },
        { "login",         no_argument,       0, 'l' },
        { "user",          required_argument, 0, 'u' },
        { "windir",        required_argument, 0, 'w' },
        { "wsldir",        required_argument, 0, 'W' },
        { 0,               no_argument,       0,  0  },
    };

    /* WinSock is initialized here */
    g_locSock = new LocalSock();
    int ret;
    Environment env;
    TerminalState termState;
    std::string distroName;
    std::string customBackendPath;
    std::string winDir;
    std::string wslDir;
    std::string userName;
    bool loginMode = false;

    if (argv[0][0] == '-')
        loginMode = true;

    int ch = 0;
    while ((ch = getopt_long(argc, argv, shortopts, longopts, nullptr)) != -1)
    {
        switch (ch)
        {
            case 0:
                /* Ignore long option. */
                break;

            case 'b':
                customBackendPath = optarg;
                if (customBackendPath.empty())
                    invalid_arg("backend");
                break;

            case 'd':
                distroName = optarg;
                if (distroName.empty())
                    invalid_arg("distribution");
                break;

            case 'e':
            {
                const char *eq = strchr(optarg, '=');
                const std::string varname = eq ?
                                            std::string(optarg, eq - optarg)
                                            : std::string(optarg);
                if (varname.empty())
                    invalid_arg("environment");

                if (eq)
                    env.set(varname, eq + 1);
                else
                    env.set(varname);
                break;
            }

            case 'h':
                usage(argv[0]);
                break;


            case 'l':
                loginMode = true;
                break;

            case 'u':
                userName = optarg;
                if (userName.empty())
                    invalid_arg("user");
                break;

            case 'w':
                winDir = optarg;
                if (winDir.empty())
                    invalid_arg("windir");
                break;

            case 'W':
                wslDir = optarg;
                if (wslDir.empty())
                    invalid_arg("wsldir");
                break;

            default:
                fatal("Try '%s --help' for more information.\n", argv[0]);
        }
    }

    SOCKET inputSocket = g_locSock->Create();
    SOCKET outputSocket = g_locSock->Create();
    SOCKET controlSocket = g_locSock->Create();

    const std::wstring wslPath = findSystemProgram(L"wsl.exe");
    const auto backendPathInfo = normalizePath(
                    findBackendProgram(customBackendPath, L"wslbridge2-backend"));
    const std::wstring backendPathWin = backendPathInfo.first;
    const std::wstring fsname = backendPathInfo.second;

    /* Prepare the backend command line. */
    std::wstring wslCmdLine;
    wslCmdLine.append(L"exec \"$(wslpath -u");
    appendWslArg(wslCmdLine, backendPathWin);
    wslCmdLine.append(L")\"");

    for (const auto &envPair : env.pairs())
    {
        appendWslArg(wslCmdLine, L"--env");
        appendWslArg(wslCmdLine, envPair.first + L"=" + envPair.second);
    }

    if (loginMode)
        appendWslArg(wslCmdLine, L"--login");

    if (!wslDir.empty())
    {
        wslCmdLine.append(L" --path \"");
        wslCmdLine.append(mbsToWcs(wslDir));
        wslCmdLine.append(L"\"");
    }

    {
        struct winsize winp = {};
        ioctl(STDIN_FILENO, TIOCGWINSZ, &winp);

        std::array<wchar_t, 1024> buffer;
        ret = swprintf(
                buffer.data(),
                buffer.size(),
                L" --cols %d --rows %d -0%d -1%d -3%d",
                winp.ws_col,
                winp.ws_row,
                g_locSock->Listen(inputSocket, 1),
                g_locSock->Listen(outputSocket, 1),
                g_locSock->Listen(controlSocket, 1));
        assert(ret > 0);
        wslCmdLine.append(buffer.data());
    }

    /* Append remaining non-option arguments as is */
    appendWslArg(wslCmdLine, L"--");
    for (int i = optind; i < argc; ++i)
        appendWslArg(wslCmdLine, mbsToWcs(argv[i]));

    /* Append wsl.exe options and its arguments */
    std::wstring cmdLine;
    cmdLine.append(L"\"");
    cmdLine.append(wslPath);
    cmdLine.append(L"\"");

    if (!distroName.empty())
    {
        cmdLine.append(L" -d ");
        cmdLine.append(mbsToWcs(distroName));
    }

   /* Taken from HKCU\Directory\Background\shell\WSL\command */
    if (!winDir.empty())
    {
        cmdLine.append(L" --cd \"");
        cmdLine.append(mbsToWcs(winDir));
        cmdLine.append(L"\"");
    }

    if (!userName.empty())
    {
        cmdLine.append(L" --user ");
        cmdLine.append(mbsToWcs(userName));
    }

    cmdLine.append(L" /bin/sh -c");
    appendWslArg(cmdLine, wslCmdLine);

    const struct PipeHandles outputPipe = createPipe();
    const struct PipeHandles errorPipe = createPipe();

    /* Initialize thread attribute list */
    HANDLE Values[2];
    Values[0] = outputPipe.wh;
    Values[1] = errorPipe.wh;

    SIZE_T AttrSize;
    LPPROC_THREAD_ATTRIBUTE_LIST AttrList = NULL;
    InitializeProcThreadAttributeList(NULL, 1, 0, &AttrSize);
    AttrList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, AttrSize);
    InitializeProcThreadAttributeList(AttrList, 1, 0, &AttrSize);
    BOOL bRes = UpdateProcThreadAttribute(
                AttrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, /* 0x20002u */
                Values, sizeof Values, NULL, NULL);
    assert(bRes != 0);

    PROCESS_INFORMATION pi = {};
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = AttrList;
    si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = outputPipe.wh;
    si.StartupInfo.hStdError = errorPipe.wh;

    ret = CreateProcessW(
            wslPath.c_str(),
            &cmdLine[0],
            NULL,
            NULL,
            TRUE,
            IsDebugMode ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW,
            NULL,
            NULL,
            &si.StartupInfo,
            &pi);
    if (!ret)
    {
        fatal("error starting wsl.exe adapter: %s\n",
            formatErrorMessage(GetLastError()).c_str());
    }

    HeapFree(GetProcessHeap(), 0, AttrList);
    CloseHandle(outputPipe.wh);
    CloseHandle(errorPipe.wh);
    ret = SetHandleInformation(pi.hProcess, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    assert(ret && "SetHandleInformation failed");

    const auto watchdog = std::thread([&]() {
        WaitForSingleObject(pi.hProcess, INFINITE);

        std::vector<char> outVec = readAllFromHandle(outputPipe.rh);
        std::vector<char> errVec = readAllFromHandle(errorPipe.rh);
        std::wstring outWide(outVec.size() / sizeof(wchar_t), L'\0');
        memcpy(&outWide[0], outVec.data(), outWide.size() * sizeof(wchar_t));
        std::string out { wcsToMbs(outWide, true) };
        std::string err { errVec.begin(), errVec.end() };
        out = stripTrailing(replaceAll(out, "Press any key to continue...", ""));
        err = stripTrailing(err);

        std::string msg;
        if (!out.empty()) {
            msg.append("note: wsl.exe output: ");
            msg.append(out);
            msg.push_back('\n');
        }
        if (!err.empty()) {
            msg.append("note: backend error output: ");
            msg.append(err);
            msg.push_back('\n');
        }

        termState.fatal("%s", msg.c_str());
    });

    g_ioSockets.inputSock = g_locSock->Accept(inputSocket);
    g_ioSockets.outputSock = g_locSock->Accept(outputSocket);
    g_ioSockets.controlSock = g_locSock->Accept(controlSocket);

    g_locSock->Close(inputSocket);
    g_locSock->Close(outputSocket);
    g_locSock->Close(controlSocket);

    /* Create thread to send window size through control socket */
    pthread_t tidResize;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    ret = pthread_create(&tidResize, nullptr, resize_window, (void *)&set);
    assert(ret == 0);

    /* Create thread to send input buffer to input socket */
    pthread_t tidInput;
    ret = pthread_create(&tidInput, nullptr, send_buffer, nullptr);
    assert(ret == 0);

    /* Create thread to send input buffer to input socket */
    pthread_t tidOutput;
    ret = pthread_create(&tidOutput, nullptr, receive_buffer, nullptr);
    assert(ret == 0);

    termState.enterRawMode();

    pthread_join(tidResize, nullptr);
    pthread_join(tidInput, nullptr);
    pthread_join(tidOutput, nullptr);

    /* cleanup */
    for (size_t i = 0; i < ARRAYSIZE(g_ioSockets.sock); i++)
        g_locSock->Close(g_ioSockets.sock[i]);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    termState.exitCleanly(0);
}