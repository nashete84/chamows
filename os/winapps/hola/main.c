/* A normal Windows console program. Build: x86_64-w64-mingw32-gcc main.c -o hola.exe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(void) {
    printf("Hola desde un programa de Windows (.exe) dentro de Chamows!\n\n");
    char *buf = malloc(64);
    strcpy(buf, "memoria pedida con malloc");
    printf("- %s\n", buf);
    free(buf);
    printf("- 7 x 6 = %d, pi = %.3f, hex = 0x%04X\n", 7 * 6, 3.14159, 48879);
    printf("- Cuenta atrás: ");
    for (int i = 5; i > 0; i--) {
        printf("%d ", i);
        Sleep(300);
    }
    printf("\n- Llevo %lu ms encendido\n", (unsigned long)GetTickCount());
    MessageBoxA(NULL, "Este cuadro lo ha pedido el .exe con MessageBoxA.", "Hola Windows", MB_OK);
    printf("\nFin.\n");
    return 0;
}
