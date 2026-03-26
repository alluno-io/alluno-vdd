/**
 * Alluno VDD Test - Add a virtual display (persists until remove_all is run)
 * Usage: add_display.exe [width] [height] [hz]
 *
 * Disables the watchdog so the display survives after this process exits.
 */

#include <stdio.h>
#include <stdlib.h>
#include "../include/alluno-vdd.h"

int main(int argc, char* argv[]) {
    UINT width = 1920, height = 1080, hz = 60;
    if (argc > 1) width = atoi(argv[1]);
    if (argc > 2) height = atoi(argv[2]);
    if (argc > 3) hz = atoi(argv[3]);

    HANDLE device = AllunoVddOpenDevice();
    if (device == INVALID_HANDLE_VALUE) {
        printf("ERROR: Could not open device. Is the driver installed?\n");
        return 1;
    }

    ALLUNO_VDD::ALLUNO_VDD_VERSION ver = {};
    if (AllunoVddGetVersion(device, &ver)) {
        printf("Driver version: %u.%u.%u\n", ver.Major, ver.Minor, ver.Patch);
    }

    // Disable watchdog so display persists after we exit
    AllunoVddSetWatchdog(device, 0);

    printf("Adding display %ux%u @ %uHz...\n", width, height, hz);
    ALLUNO_VDD::ALLUNO_VDD_ADD_RESULT result = {};
    if (!AllunoVddAddDisplay(device, width, height, hz, "Alluno Display", 8, 0, &result)) {
        printf("ERROR: Failed to add display (GetLastError=%lu)\n", GetLastError());
        AllunoVddCloseDevice(device);
        return 1;
    }
    printf("Display added! TargetId=%u\n", result.TargetId);

    ALLUNO_VDD::ALLUNO_VDD_LIST_RESULT list = {};
    if (AllunoVddListDisplays(device, &list)) {
        printf("Active displays: %u\n", list.Count);
        for (UINT i = 0; i < list.Count; i++) {
            printf("  [%u] %ux%u @%uHz\n", i,
                list.Displays[i].Width, list.Displays[i].Height,
                list.Displays[i].RefreshRate);
        }
    }

    printf("Display will persist until remove_all.exe is run.\n");
    AllunoVddCloseDevice(device);
    return 0;
}
