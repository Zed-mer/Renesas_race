#include <errno.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *interface_name = argc > 1 ? argv[1] : "eth0";
    struct ethtool_cmd command = { .cmd = ETHTOOL_GSET };
    struct ifreq request;
    int socket_fd;

    memset(&request, 0, sizeof(request));
    if (strlen(interface_name) >= sizeof(request.ifr_name))
    {
        fprintf(stderr, "interface name is too long\n");
        return 2;
    }
    strcpy(request.ifr_name, interface_name);
    request.ifr_data = (void *)&command;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        perror("socket");
        return 3;
    }
    if (ioctl(socket_fd, SIOCETHTOOL, &request) < 0)
    {
        perror("ETHTOOL_GSET");
        close(socket_fd);
        return 4;
    }

    printf("before supported=0x%08x advertising=0x%08x\n",
           command.supported,
           command.advertising);
    command.advertising |= ADVERTISED_Pause | ADVERTISED_Asym_Pause;
    command.cmd = ETHTOOL_SSET;
    if (ioctl(socket_fd, SIOCETHTOOL, &request) < 0)
    {
        perror("ETHTOOL_SSET");
        close(socket_fd);
        return 5;
    }

    command.cmd = ETHTOOL_GSET;
    if (ioctl(socket_fd, SIOCETHTOOL, &request) < 0)
    {
        perror("ETHTOOL_GSET verify");
        close(socket_fd);
        return 6;
    }
    close(socket_fd);

    printf("after  supported=0x%08x advertising=0x%08x\n",
           command.supported,
           command.advertising);
    return 0;
}
