/*
 * Copyright 2014 Akihiro Sagawa
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <windows.h>
#include "wine/test.h"

#define lok ok_(__FILE__,line)
#define KEY_BASE "Software\\Wine\\reg_test"
#define REG_EXIT_SUCCESS 0
#define REG_EXIT_FAILURE 1
#define TODO_REG_TYPE    (0x0001u)
#define TODO_REG_SIZE    (0x0002u)
#define TODO_REG_DATA    (0x0004u)

#define run_reg_exe(c,r) run_reg_exe_(__LINE__,c,r)
static BOOL run_reg_exe_(unsigned line, const char *cmd, DWORD *rc)
{
    STARTUPINFOA si = {sizeof(STARTUPINFOA)};
    PROCESS_INFORMATION pi;
    BOOL bret;
    DWORD ret;
    char cmdline[256];

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = INVALID_HANDLE_VALUE;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    strcpy(cmdline, cmd);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return FALSE;

    ret = WaitForSingleObject(pi.hProcess, 10000);
    if (ret == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);

    bret = GetExitCodeProcess(pi.hProcess, rc);
    lok(bret, "GetExitCodeProcess failed: %d\n", GetLastError());

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return bret;
}

#define verify_reg(k,v,t,d,s,todo) verify_reg_(__LINE__,k,v,t,d,s,todo)
static void verify_reg_(unsigned line, HKEY hkey, const char* value,
                        DWORD exp_type, const void *exp_data, DWORD exp_size, DWORD todo)
{
    DWORD type, size;
    BYTE data[256];
    LONG err;

    size = sizeof(data);
    memset(data, 0xdd, size);
    err = RegQueryValueExA(hkey, value, NULL, &type, data, &size);
    lok(err == ERROR_SUCCESS, "RegQueryValueEx failed: got %d\n", err);
    if (err != ERROR_SUCCESS)
        return;

    todo_wine_if (todo & TODO_REG_TYPE)
        lok(type == exp_type, "got wrong type %d, expected %d\n", type, exp_type);
    todo_wine_if (todo & TODO_REG_SIZE)
        lok(size == exp_size, "got wrong size %d, expected %d\n", size, exp_size);
    todo_wine_if (todo & TODO_REG_DATA)
        lok(memcmp(data, exp_data, size) == 0, "got wrong data\n");
}

static void test_add(void)
{
    HKEY hkey, subkey;
    LONG err;
    DWORD r, dword, type, size;
    char buffer[22];

    run_reg_exe("reg add", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add /?", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE);
    ok(err == ERROR_SUCCESS || err == ERROR_FILE_NOT_FOUND, "got %d\n", err);

    err = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_BASE, 0, KEY_READ, &hkey);
    ok(err == ERROR_FILE_NOT_FOUND, "got %d\n", err);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    err = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_BASE, 0, KEY_READ, &hkey);
    ok(err == ERROR_SUCCESS, "key creation failed, got %d\n", err);

    /* Test empty type */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v emptyType /t \"\" /d WineTest /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);

    /* Test input key formats */
    run_reg_exe("reg add \\HKCU\\" KEY_BASE "\\keytest0 /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE "\\keytest0");
    ok(err == ERROR_FILE_NOT_FOUND, "got exit code %d\n", r);

    run_reg_exe("reg add \\\\HKCU\\" KEY_BASE "\\keytest1 /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE "\\keytest1");
    ok(err == ERROR_FILE_NOT_FOUND, "got exit code %d\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE "\\keytest2\\\\ /f", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */),
        "got exit code %u\n", r);
    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE "\\keytest2");
    ok(err == ERROR_FILE_NOT_FOUND || broken(err == ERROR_SUCCESS /* WinXP */),
        "got exit code %d\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE "\\keytest3\\ /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    err = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_BASE "\\keytest3", 0, KEY_READ, &subkey);
    ok(err == ERROR_SUCCESS, "key creation failed, got %d\n", err);
    RegCloseKey(subkey);
    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE "\\keytest3");
    ok(err == ERROR_SUCCESS, "got exit code %d\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE "\\keytest4 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    err = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_BASE "\\keytest4", 0, KEY_READ, &subkey);
    ok(err == ERROR_SUCCESS, "key creation failed, got %d\n", err);
    RegCloseKey(subkey);
    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE "\\keytest4");
    ok(err == ERROR_SUCCESS, "got exit code %d\n", r);

    /* REG_NONE */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v none0 /d deadbeef /t REG_NONE /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d\n", r);
    verify_reg(hkey, "none0", REG_NONE, "d\0e\0a\0d\0b\0e\0e\0f\0\0", 18, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v none1 /t REG_NONE /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "none1", REG_NONE, "\0", 2, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /t REG_NONE /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, NULL, REG_NONE, "\0", 2, 0);

    /* REG_SZ */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /d WineTest /f", &r);
    ok(r == REG_EXIT_SUCCESS || broken(r == REG_EXIT_FAILURE /* WinXP */),
       "got exit code %d, expected 0\n", r);
    if (r == REG_EXIT_SUCCESS)
        verify_reg(hkey, "", REG_SZ, "WineTest", 9, 0);
    else
        win_skip("broken reg.exe detected\n");

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v test /d deadbeef /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    verify_reg(hkey, "test", REG_SZ, "deadbeef", 9, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v test /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    verify_reg(hkey, "test", REG_SZ, "", 1, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v test1 /t REG_SZ /f /d", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKEY_CURRENT_USER\\" KEY_BASE " /ve /d WineTEST /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    verify_reg(hkey, "", REG_SZ, "WineTEST", 9, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_SZ /v test2 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    verify_reg(hkey, "test2", REG_SZ, "", 1, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_SZ /v test3 /f /d \"\"", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    verify_reg(hkey, "test3", REG_SZ, "", 1, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, NULL, REG_SZ, "", 1, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /t REG_SZ /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, NULL, REG_SZ, "", 1, 0);

    /* REG_EXPAND_SZ */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v expand0 /t REG_EXpand_sz /d \"dead%PATH%beef\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "expand0", REG_EXPAND_SZ, "dead%PATH%beef", 15, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v expand1 /t REG_EXpand_sz /d \"dead^%PATH^%beef\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "expand1", REG_EXPAND_SZ, "dead^%PATH^%beef", 17, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_EXPAND_SZ /v expand2 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "expand2", REG_EXPAND_SZ, "", 1, 0);

    run_reg_exe("reg add HKEY_CURRENT_USER\\" KEY_BASE " /ve /t REG_EXPAND_SZ /d WineTEST /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "", REG_EXPAND_SZ, "WineTEST", 9, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_EXPAND_SZ /v expand3 /f /d \"\"", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "expand3", REG_EXPAND_SZ, "", 1, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /t REG_EXPAND_SZ /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, NULL, REG_EXPAND_SZ, "", 1, 0);

    /* REG_BINARY */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_BINARY /v bin0 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "bin0", REG_BINARY, buffer, 0, 0);

    run_reg_exe("reg add HKEY_CURRENT_USER\\" KEY_BASE " /ve /t REG_BINARY /d deadbeef /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    dword = 0xefbeadde;
    verify_reg(hkey, "", REG_BINARY, &dword, sizeof(DWORD), 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_BINARY /v bin1 /f /d 0xDeAdBeEf", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_BINARY /v bin2 /f /d x01", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_BINARY /v bin3 /f /d 01x", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_BINARY /v bin4 /f /d DeAdBeEf0DD", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    /* Remaining nibble prefixed */
    buffer[0] = 0x0d; buffer[1] = 0xea; buffer[2] = 0xdb;
    buffer[3] = 0xee; buffer[4] = 0xf0; buffer[5] = 0xdd;
    /* Remaining nibble suffixed on winXP */
    buffer[6] = 0xde; buffer[7] = 0xad; buffer[8] = 0xbe;
    buffer[9] = 0xef; buffer[10] = 0x0d; buffer[11] = 0xd0;
    size = 6;
    err = RegQueryValueExA(hkey, "bin4", NULL, &type, (void *) (buffer+12), &size);
    ok(err == ERROR_SUCCESS, "RegQueryValueEx failed: got %d\n", err);
    ok(type == REG_BINARY, "got wrong type %u\n", type);
    ok(size == 6, "got wrong size %u\n", size);
    ok(memcmp(buffer, buffer+12, 6) == 0 ||
        broken(memcmp(buffer+6, buffer+12, 6) == 0 /* WinXP */), "got wrong data\n");

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_BINARY /v bin5 /d \"\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "bin5", REG_BINARY, buffer, 0, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v bin6 /t REG_BINARY /f /d", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /t REG_BINARY /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, NULL, REG_BINARY, buffer, 0, 0);

    /* REG_DWORD */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_DWORD /f /d 12345678", &r);
    ok(r == REG_EXIT_SUCCESS || broken(r == REG_EXIT_FAILURE /* WinXP */),
       "got exit code %d, expected 0\n", r);
    dword = 12345678;
    if (r == REG_EXIT_SUCCESS)
        verify_reg(hkey, "", REG_DWORD, &dword, sizeof(dword), 0);
    else
        win_skip("broken reg.exe detected\n");

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword0 /t REG_DWORD /f /d", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword1 /t REG_DWORD /f", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */),
       "got exit code %d, expected 1\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword2 /t REG_DWORD /d zzz /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword3 /t REG_DWORD /d deadbeef /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword4 /t REG_DWORD /d 123xyz /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword5 /t reg_dword /d 12345678 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    dword = 12345678;
    verify_reg(hkey, "dword5", REG_DWORD, &dword, sizeof(dword), 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword6 /t REG_DWORD /D 0123 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    size = sizeof(dword);
    err = RegQueryValueExA(hkey, "dword6", NULL, &type, (LPBYTE)&dword, &size);
    ok(err == ERROR_SUCCESS, "RegQueryValueEx failed: got %d\n", err);
    ok(type == REG_DWORD, "got wrong type %d, expected %d\n", type, REG_DWORD);
    ok(size == sizeof(DWORD), "got wrong size %d, expected %d\n", size, (int)sizeof(DWORD));
    ok(dword == 123 || broken(dword == 0123 /* WinXP */), "got wrong data %d, expected 123\n", dword);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword7 /t reg_dword /d 0xabcdefg /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword8 /t REG_dword /d 0xdeadbeef /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    dword = 0xdeadbeef;
    verify_reg(hkey, "dword8", REG_DWORD, &dword, sizeof(dword), 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_DWORD /v dword9 /f /d -1", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */), "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_DWORD /v dword10 /f /d -0x1", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */), "got exit code %u\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword8 /t REG_dword /d 0x01ffffffff /f", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */), "got exit code %d\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword12 /t REG_DWORD /d 0xffffffff /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    dword = ~0u;
    verify_reg(hkey, "dword12", REG_DWORD, &dword, sizeof(dword), 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword13 /t REG_DWORD /d 00x123 /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword14 /t REG_DWORD /d 0X123 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    dword = 0x123;
    verify_reg(hkey, "dword14", REG_DWORD, &dword, sizeof(dword), 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dword15 /t REG_DWORD /d 4294967296 /f", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */), "got exit code %u\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /t REG_DWORD /f", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */), "got exit code %u\n", r);

    /* REG_DWORD_LITTLE_ENDIAN */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v DWORD_LE /t REG_DWORD_LITTLE_ENDIAN /d 456 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    dword = 456;
    verify_reg(hkey, "DWORD_LE", REG_DWORD_LITTLE_ENDIAN, &dword, sizeof(dword), 0);

    /* REG_DWORD_BIG_ENDIAN */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v DWORD_BE /t REG_DWORD_BIG_ENDIAN /d 456 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    dword = 456;
    verify_reg(hkey, "DWORD_BE", REG_DWORD_BIG_ENDIAN, &dword, sizeof(dword), 0);
    /* REG_DWORD_BIG_ENDIAN is broken in every version of windows. It behaves like
     * an ordinary REG_DWORD - that is little endian. GG */

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v DWORD_BE2 /t REG_DWORD_BIG_ENDIAN /f /d", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v DWORD_BE3 /t REG_DWORD_BIG_ENDIAN /f", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */), "got exit code %u\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /t REG_DWORD_BIG_ENDIAN /f", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */), "got exit code %u\n", r);

    /* REG_MULTI_SZ */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi0 /t REG_MULTI_SZ /d \"three\\0little\\0strings\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    memcpy(buffer, "three\0little\0strings\0", 22);
    verify_reg(hkey, "multi0", REG_MULTI_SZ, buffer, 22, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi1 /s \"#\" /d \"three#little#strings\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi1", REG_MULTI_SZ, buffer, 22, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi2 /d \"\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi2", REG_MULTI_SZ, &buffer[21], 1, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi3 /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u\n", r);
    verify_reg(hkey, "multi3", REG_MULTI_SZ, &buffer[21], 1, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi4 /s \"#\" /d \"threelittlestrings\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi4", REG_MULTI_SZ, "threelittlestrings\0", 20, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi5 /s \"#randomgibberish\" /d \"three#little#strings\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi6 /s \"\\0\" /d \"three\\0little\\0strings\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi7 /s \"\" /d \"three#little#strings\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi8 /s \"#\" /d \"##\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi9 /s \"#\" /d \"two##strings\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi10 /s \"#\" /d \"#a\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi11 /s \"#\" /d \"a#\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    buffer[0]='a'; buffer[1]=0; buffer[2]=0;
    verify_reg(hkey, "multi11", REG_MULTI_SZ, buffer, 3, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi12 /t REG_MULTI_SZ /f /d", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi13 /t REG_MULTI_SZ /f /s", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi14 /t REG_MULTI_SZ /d \"\\0a\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi15 /t REG_MULTI_SZ /d \"a\\0\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi15", REG_MULTI_SZ, buffer, 3, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /t REG_MULTI_SZ /v multi16 /d \"two\\0\\0strings\" /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi17 /t REG_MULTI_SZ /s \"#\" /d \"#\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    buffer[0] = 0; buffer[1] = 0;
    verify_reg(hkey, "multi17", REG_MULTI_SZ, buffer, 2, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi18 /t REG_MULTI_SZ /d \"\\0\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi18", REG_MULTI_SZ, buffer, 2, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi19 /t REG_MULTI_SZ /s \"#\" /d \"two\\0#strings\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi19", REG_MULTI_SZ, "two\\0\0strings\0", 15, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi20 /t REG_MULTI_SZ /s \"#\" /d \"two#\\0strings\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi20", REG_MULTI_SZ, "two\0\\0strings\0", 15, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v multi21 /t REG_MULTI_SZ /s \"#\" /d \"two\\0\\0strings\" /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, "multi21", REG_MULTI_SZ, "two\\0\\0strings\0", 16, 0);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /ve /t REG_MULTI_SZ /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %u, expected 0\n", r);
    verify_reg(hkey, NULL, REG_MULTI_SZ, buffer, 1, 0);

    RegCloseKey(hkey);

    /* Test duplicate switches */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dup1 /t REG_DWORD /d 123 /f /t REG_SZ", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS /* WinXP */),
       "got exit code %u, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v dup2 /t REG_DWORD /d 123 /f /d 456", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    /* Test invalid switches */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v invalid1 /a", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v invalid2 /ae", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v invalid3 /", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v invalid4 -", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u, expected 1\n", r);

    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE);
    ok(err == ERROR_SUCCESS, "got %d\n", err);
}

static void test_delete(void)
{
    HKEY hkey, hsubkey;
    LONG err;
    DWORD r;
    const DWORD deadbeef = 0xdeadbeef;

    run_reg_exe("reg delete", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg delete /?", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    err = RegCreateKeyExA(HKEY_CURRENT_USER, KEY_BASE, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL);
    ok(err == ERROR_SUCCESS, "got %d\n", err);

    err = RegSetValueExA(hkey, "foo", 0, REG_DWORD, (LPBYTE)&deadbeef, sizeof(deadbeef));
    ok(err == ERROR_SUCCESS, "got %d\n" ,err);

    err = RegSetValueExA(hkey, "bar", 0, REG_DWORD, (LPBYTE)&deadbeef, sizeof(deadbeef));
    ok(err == ERROR_SUCCESS, "got %d\n" ,err);

    err = RegSetValueExA(hkey, "", 0, REG_DWORD, (LPBYTE)&deadbeef, sizeof(deadbeef));
    ok(err == ERROR_SUCCESS, "got %d\n" ,err);

    err = RegCreateKeyExA(hkey, "subkey", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hsubkey, NULL);
    ok(err == ERROR_SUCCESS, "got %d\n" ,err);
    RegCloseKey(hsubkey);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /v bar /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    err = RegQueryValueExA(hkey, "bar", NULL, NULL, NULL, NULL);
    ok(err == ERROR_FILE_NOT_FOUND, "got %d\n", err);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /ve /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    err = RegQueryValueExA(hkey, "", NULL, NULL, NULL, NULL);
    ok(err == ERROR_FILE_NOT_FOUND, "got %d, expected 2\n", err);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /va /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    err = RegQueryValueExA(hkey, "foo", NULL, NULL, NULL, NULL);
    ok(err == ERROR_FILE_NOT_FOUND, "got %d\n", err);
    err = RegOpenKeyExA(hkey, "subkey", 0, KEY_READ, &hsubkey);
    ok(err == ERROR_SUCCESS, "got %d\n", err);
    RegCloseKey(hsubkey);
    RegCloseKey(hkey);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /f", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);
    err = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_BASE, 0, KEY_READ, &hkey);
    ok(err == ERROR_FILE_NOT_FOUND, "got %d\n", err);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %u\n", r);
}

static void test_query(void)
{
    DWORD r;
    HKEY key, subkey;
    LONG err;
    const char hello[] = "Hello";
    const char world[] = "World";
    const char empty1[] = "Empty1";
    const char empty2[] = "Empty2";
    const DWORD dword1 = 0x123;
    const DWORD dword2 = 0xabc;

    run_reg_exe("reg query", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg query /?", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    /* Create a test key */
    err = RegCreateKeyExA(HKEY_CURRENT_USER, KEY_BASE, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL);
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /ve", &r);
    ok(r == REG_EXIT_SUCCESS || broken(r == REG_EXIT_FAILURE /* WinXP */),
       "got exit code %d, expected 0\n", r);

    err = RegSetValueExA(key, "Test", 0, REG_SZ, (BYTE *)hello, sizeof(hello));
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    err = RegSetValueExA(key, "Wine", 0, REG_DWORD, (BYTE *)&dword1, sizeof(dword1));
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    err = RegSetValueExA(key, NULL, 0, REG_SZ, (BYTE *)empty1, sizeof(empty1));
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    run_reg_exe("reg query HKCU\\" KEY_BASE, &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /v", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /v Missing", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /v Test", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /v Wine", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /ve", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    /* Create a test subkey */
    err = RegCreateKeyExA(key, "Subkey", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &subkey, NULL);
    ok(err == ERROR_SUCCESS, "got %d\n", err);

    err = RegSetValueExA(subkey, "Test", 0, REG_SZ, (BYTE *)world, sizeof(world));
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    err = RegSetValueExA(subkey, "Wine", 0, REG_DWORD, (BYTE *)&dword2, sizeof(dword2));
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    err = RegSetValueExA(subkey, NULL, 0, REG_SZ, (BYTE *)empty2, sizeof(empty2));
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    err = RegCloseKey(subkey);
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    run_reg_exe("reg query HKCU\\" KEY_BASE "\\subkey", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE "\\subkey /v Test", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE "\\subkey /v Wine", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE "\\subkey /ve", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    /* Test recursion */
    run_reg_exe("reg query HKCU\\" KEY_BASE " /s", &r);
    ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /v Test /s", &r);
    ok(r == REG_EXIT_SUCCESS || r == REG_EXIT_FAILURE /* WinXP */,
       "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /v Wine /s", &r);
    ok(r == REG_EXIT_SUCCESS || r == REG_EXIT_FAILURE /* WinXP */,
       "got exit code %d, expected 0\n", r);

    run_reg_exe("reg query HKCU\\" KEY_BASE " /ve /s", &r);
    ok(r == REG_EXIT_SUCCESS || r == REG_EXIT_FAILURE /* WinXP */,
       "got exit code %d, expected 0\n", r);

    /* Clean-up, then query */
    err = RegDeleteKeyA(key, "subkey");
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    err = RegCloseKey(key);
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    run_reg_exe("reg query HKCU\\" KEY_BASE "\\subkey", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE);
    ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    run_reg_exe("reg query HKCU\\" KEY_BASE, &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);
}

static void test_v_flags(void)
{
    DWORD r;

    run_reg_exe("reg add HKCU\\" KEY_BASE " /v Wine /ve", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /v Wine /ve", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /v Wine /va", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /ve /va", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    /* No /v argument */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg add HKCU\\" KEY_BASE " /d Test /f /v", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /v", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /f /v", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    /* Multiple /v switches */
    run_reg_exe("reg add HKCU\\" KEY_BASE " /v Wine /t REG_DWORD /d 0x1 /v Test /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg delete HKCU\\" KEY_BASE " /v Wine /v Test /f", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);
}

static BOOL write_reg_file(const char *value, char *tmp_name)
{
    static const char regedit4[] = "REGEDIT4";
    static const char key[] = "[HKEY_CURRENT_USER\\" KEY_BASE "]";
    char file_data[MAX_PATH], tmp_path[MAX_PATH];
    HANDLE hfile;
    DWORD written;
    BOOL ret;

    sprintf(file_data, "%s\n\n%s\n%s\n", regedit4, key, value);

    GetTempPathA(MAX_PATH, tmp_path);
    GetTempFileNameA(tmp_path, "reg", 0, tmp_name);

    hfile = CreateFileA(tmp_name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (hfile == INVALID_HANDLE_VALUE)
        return FALSE;

    ret = WriteFile(hfile, file_data, strlen(file_data), &written, NULL);
    CloseHandle(hfile);
    return ret;
}

#define test_import_str(c,r) test_import_str_(__LINE__,c,r)
static BOOL test_import_str_(unsigned line, const char *file_contents, DWORD *rc)
{
    HANDLE regfile;
    DWORD written;
    BOOL ret;

    regfile = CreateFileA("test.reg", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    lok(regfile != INVALID_HANDLE_VALUE, "Failed to create test.reg file\n");
    if(regfile == INVALID_HANDLE_VALUE)
        return FALSE;

    ret = WriteFile(regfile, file_contents, strlen(file_contents), &written, NULL);
    lok(ret, "WriteFile failed: %u\n", GetLastError());
    CloseHandle(regfile);

    run_reg_exe("reg import test.reg", rc);

    ret = DeleteFileA("test.reg");
    lok(ret, "DeleteFile failed: %u\n", GetLastError());

    return ret;
}

#define test_import_wstr(c,r) test_import_wstr_(__LINE__,c,r)
static BOOL test_import_wstr_(unsigned line, const char *file_contents, DWORD *rc)
{
    int len, memsize;
    WCHAR *wstr;
    HANDLE regfile;
    DWORD written;
    BOOL ret;

    len = MultiByteToWideChar(CP_UTF8, 0, file_contents, -1, NULL, 0);
    memsize = len * sizeof(WCHAR);
    wstr = HeapAlloc(GetProcessHeap(), 0, memsize);
    if (!wstr) return FALSE;
    MultiByteToWideChar(CP_UTF8, 0, file_contents, -1, wstr, memsize);

    regfile = CreateFileA("test.reg", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    lok(regfile != INVALID_HANDLE_VALUE, "Failed to create test.reg file\n");
    if(regfile == INVALID_HANDLE_VALUE)
        return FALSE;

    ret = WriteFile(regfile, wstr, memsize, &written, NULL);
    lok(ret, "WriteFile failed: %u\n", GetLastError());
    CloseHandle(regfile);

    HeapFree(GetProcessHeap(), 0, wstr);

    run_reg_exe("reg import test.reg", rc);

    ret = DeleteFileA("test.reg");
    lok(ret, "DeleteFile failed: %u\n", GetLastError());

    return ret;
}

static void test_import(void)
{
    DWORD r, dword = 0x123;
    char test1_reg[MAX_PATH], test2_reg[MAX_PATH];
    char cmdline[MAX_PATH];
    char test_string[] = "Test string";
    HKEY hkey;
    LONG err;

    run_reg_exe("reg import", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    run_reg_exe("reg import /?", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    run_reg_exe("reg import missing.reg", &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    /* Create test files */
    ok(write_reg_file("\"Wine\"=dword:00000123", test1_reg), "Failed to write registry file\n");
    ok(write_reg_file("@=\"Test string\"", test2_reg), "Failed to write registry file\n");

    sprintf(cmdline, "reg import %s", test1_reg);
    run_reg_exe(cmdline, &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    sprintf(cmdline, "reg import %s", test2_reg);
    run_reg_exe(cmdline, &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    err = RegOpenKeyExA(HKEY_CURRENT_USER, KEY_BASE, 0, KEY_READ, &hkey);
    todo_wine ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    todo_wine verify_reg(hkey, "Wine", REG_DWORD, &dword, sizeof(dword), 0);
    todo_wine verify_reg(hkey, "", REG_SZ, test_string, sizeof(test_string), 0);

    err = RegCloseKey(hkey);
    todo_wine ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    err = RegDeleteKeyA(HKEY_CURRENT_USER, KEY_BASE);
    todo_wine ok(err == ERROR_SUCCESS, "got %d, expected 0\n", err);

    sprintf(cmdline, "reg import %s %s", test1_reg, test2_reg);
    run_reg_exe(cmdline, &r);
    ok(r == REG_EXIT_FAILURE, "got exit code %d, expected 1\n", r);

    DeleteFileA(test1_reg);
    DeleteFileA(test2_reg);

    /* Test file contents */
    test_import_str("regedit\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_str("regedit4\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_str("REGEDIT\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("REGEDIT4\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str(" REGEDIT4\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("\tREGEDIT4\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("\nREGEDIT4\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_str("AREGEDIT4\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_str("1REGEDIT4\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_str("REGEDIT3\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("REGEDIT5\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("REGEDIT9\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("REGEDIT 4\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("REGEDIT4 FOO\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_str("Windows Registry Editor Version 4.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_str("Windows Registry Editor Version 5.00\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    /* Test file contents - Unicode */
    test_import_wstr("Windows Registry Editor Version 4.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_wstr("Windows Registry Editor Version 5.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_wstr("\uFEFFWindows Registry Editor Version 5\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_wstr("\uFEFFWindows Registry Editor Version 5.00\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_wstr("\uFEFFWINDOWS Registry Editor Version 5.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_wstr(" \uFEFFWindows Registry Editor Version 5.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_wstr("\t\uFEFFWindows Registry Editor Version 5.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_wstr("\n\uFEFFWindows Registry Editor Version 5.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);

    test_import_wstr("\uFEFF Windows Registry Editor Version 5.00\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_wstr("\uFEFF\tWindows Registry Editor Version 5.00\n", &r);
    todo_wine ok(r == REG_EXIT_SUCCESS, "got exit code %d, expected 0\n", r);

    test_import_wstr("\uFEFF\nWindows Registry Editor Version 5.00\n", &r);
    ok(r == REG_EXIT_FAILURE || broken(r == REG_EXIT_SUCCESS) /* WinXP */,
       "got exit code %d, expected 1\n", r);
}

START_TEST(reg)
{
    DWORD r;
    if (!run_reg_exe("reg.exe /?", &r)) {
        win_skip("reg.exe not available, skipping reg.exe tests\n");
        return;
    }

    test_add();
    test_delete();
    test_query();
    test_v_flags();
    test_import();
}
