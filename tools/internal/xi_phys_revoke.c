
#define _GNU_SOURCE
#include "dom0_defs.h"

int main(int argc, char *argv[])
{
    privcmd_blkmsg_t blkmsg;
    xp_disk_t        xpd;

    if ( argc != 5 )
    {
	fprintf(stderr, "Usage: xi_physdev_revoke <domain> "
                "<device> <start sector> <n_sectors>\n");
	return 1;
    }

    xpd.mode       = 0;
    xpd.domain     = atol(argv[1]);
    xpd.device     = xldev_to_physdev(atol(argv[2]));
    xpd.start_sect = atol(argv[3]);
    xpd.n_sectors  = atol(argv[4]);

    if ( xpd.device == 0 )
    {
        ERROR("Unrecognised device");
        return 1;
    }

    blkmsg.op       = XEN_BLOCK_PHYSDEV_GRANT;
    blkmsg.buf      = &xpd;
    blkmsg.buf_size = sizeof(xpd);

    if ( do_xen_blkmsg(&blkmsg) < 0 )
        return 1;

    return 0;
}
