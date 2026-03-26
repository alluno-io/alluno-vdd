/**
 * Alluno VDD Test - Remove all virtual displays
 * Usage: remove_all.exe
 */

#include <stdio.h>
#include "../include/alluno-vdd.h"

int main() {
    HANDLE device = AllunoVddOpenDevice();
    if (device == INVALID_HANDLE_VALUE) {
        printf("ERROR: Could not open device. Is the driver installed?\n");
        return 1;
    }

    ALLUNO_VDD::ALLUNO_VDD_LIST_RESULT list = {};
    if (AllunoVddListDisplays(device, &list)) {
        printf("Active displays before: %u\n", list.Count);
    }

    if (list.Count == 0) {
        printf("No displays to remove.\n");
        AllunoVddCloseDevice(device);
        return 0;
    }

    printf("Removing all displays...\n");
    if (AllunoVddRemoveAll(device)) {
        printf("All displays removed.\n");
    } else {
        printf("ERROR: Failed to remove displays (GetLastError=%lu)\n", GetLastError());
        AllunoVddCloseDevice(device);
        return 1;
    }

    if (AllunoVddListDisplays(device, &list)) {
        printf("Active displays after: %u\n", list.Count);
    }

    AllunoVddCloseDevice(device);
    return 0;
}
