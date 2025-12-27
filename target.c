#include <windows.h>
#include <stdio.h>

DWORD WINAPI AutoCloseMsgBox(LPVOID lpParam) {
    int delayMs = (int)(INT_PTR)lpParam;

    Sleep(delayMs);

    HWND hWnd = FindWindowA("#32770", "Title"); // classe standard des MessageBox
    if (hWnd) {
        PostMessageA(hWnd, WM_CLOSE, 0, 0);
    }

    return 0;
}

int main() {

    while (TRUE) {

        // lance un thread qui fermera la box dans 1 secondes
        CreateThread(NULL, 0, AutoCloseMsgBox, (LPVOID)1000, 0, NULL);

        MessageBoxA(
            NULL,
            "Hello",
            "Title",
            MB_OK
        );

        Sleep(2000);
    }

    return 0;
}
