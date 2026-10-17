#ifndef BTINIT_H
#define BTINIT_H
// powers the WCN3990, downloads rampatch + NVM, returns the UART descriptor ready for HCI H4
int btinit_open(const char *tty, const char *fwdir, void (*log)(const char *));
int btinit_power(int on);
int btinit_send_cmd_sync(int fd, uint16_t opcode, const uint8_t *param, int len);
void btinit_shutdown(int fd);
#endif
