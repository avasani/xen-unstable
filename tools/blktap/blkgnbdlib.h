/* blkgnbdlib.h
 *
 * gndb image-backed block device.
 * 
 * (c) 2004 Andrew Warfield.
 *
 * Xend has been modified to use an amorfs:[fsid] disk tag.
 * This will show up as device type (maj:240,min:0) = 61440.
 *
 * The fsid is placed in the sec_start field of the disk extent.
 */

int gnbd_control(control_msg_t *msg);
int gnbd_request(blkif_request_t *req);
int gnbd_response(blkif_response_t *rsp); /* noop */
void gnbd_init(void);
