#include "config.h"
#include "freebsd_wake_beacon.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int freebsd_wake_beacon_send(unsigned int firmware_version) {
#if ENABLE_FREEBSD_WAKE_BEACON
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    printf("[freebsd-wake] socket failed errno=%d\n", errno);
    return -1;
  }

  int yes = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
    printf("[freebsd-wake] SO_BROADCAST failed errno=%d\n", errno);
    close(fd);
    return -1;
  }

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(FREEBSD_WAKE_BEACON_PORT);
  dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

  char payload[160];
  int len = snprintf(payload, sizeof(payload),
                     "PS5FREEBSD_ARMED v1 token=%s fw=%04x",
                     FREEBSD_WAKE_BEACON_TOKEN, firmware_version);
  if (len <= 0 || len >= (int)sizeof(payload)) {
    printf("[freebsd-wake] payload formatting failed\n");
    close(fd);
    return -1;
  }

  int ok = 0;
  for (int i = 0; i < FREEBSD_WAKE_BEACON_REPEATS; i++) {
    ssize_t sent = sendto(fd, payload, (size_t)len, 0,
                          (const struct sockaddr *)&dst, sizeof(dst));
    if (sent == len) {
      ok = 1;
      printf("[freebsd-wake] beacon %d/%d sent: %s\n", i + 1,
             FREEBSD_WAKE_BEACON_REPEATS, payload);
    } else {
      printf("[freebsd-wake] beacon %d/%d failed errno=%d sent=%zd\n", i + 1,
             FREEBSD_WAKE_BEACON_REPEATS, errno, sent);
    }
    if (i + 1 < FREEBSD_WAKE_BEACON_REPEATS) {
      usleep(FREEBSD_WAKE_BEACON_INTERVAL_US);
    }
  }

  close(fd);
  return ok ? 0 : -1;
#else
  (void)firmware_version;
  return 0;
#endif
}
