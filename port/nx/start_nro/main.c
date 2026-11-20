/* A small NRO for test runs: nxlink sends this instead of the 24 MB game, which over Wi-Fi often
   fails partway and cannot resume, and it starts the game already on the card (put there by FTP,
   which can resume). The game then logs to dhd.log on the card. */
#include <switch.h>

#define GAME "sdmc:/switch/downhill-revived/downhill-revived.nro"

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    envSetNextLoad(GAME, "\"" GAME "\"");
    return 0;
}
