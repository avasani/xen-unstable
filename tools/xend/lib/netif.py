
###################################################################
## xend/netif.py -- Network-interface management functions for Xend
## Copyright (c) 2004, K A Fraser (University of Cambridge)
###################################################################

import errno, random, re, os, select, signal, socket, struct, sys
import xend.main, xend.console, xend.manager, xend.utils, Xc

CMSG_NETIF_BE = 3
CMSG_NETIF_FE = 4
CMSG_NETIF_FE_INTERFACE_STATUS_CHANGED =  0
CMSG_NETIF_FE_DRIVER_STATUS_CHANGED    = 32
CMSG_NETIF_BE_DRIVER_STATUS_CHANGED    = 32
CMSG_NETIF_FE_INTERFACE_CONNECT        = 33
CMSG_NETIF_FE_INTERFACE_DISCONNECT     = 34
CMSG_NETIF_BE_CREATE      = 0
CMSG_NETIF_BE_DESTROY     = 1
CMSG_NETIF_BE_CONNECT     = 2
CMSG_NETIF_BE_DISCONNECT  = 3

NETIF_DRIVER_STATUS_DOWN  = 0
NETIF_DRIVER_STATUS_UP    = 1

pendmsg = None
pendaddr = None

recovery = False # Is a recovery in progress?
be_port  = None  # Port object for backend domain

def backend_tx_req(msg):
    if xend.netif.be_port.space_to_write_request():
        xend.netif.be_port.write_request(msg)
        xend.netif.be_port.notify()
    else:
        xend.netif.pendmsg = msg

def backend_rx_req(port, msg):
    port.write_response(msg)
    subtype = (msg.get_header())['subtype']
    print "Received netif-be request, subtype %d" % subtype
    if subtype == CMSG_NETIF_BE_DRIVER_STATUS_CHANGED:
        pl = msg.get_payload()
        status = pl['status']
        if status == NETIF_DRIVER_STATUS_UP:
            if xend.netif.recovery:
                print "New netif backend now UP, notifying guests:"
                for netif_key in interface.list.keys():
                    netif = interface.list[netif_key]
                    netif.create()
                    print "  Notifying %d" % netif.dom
                    msg = xend.utils.message(
                        CMSG_NETIF_FE,
                        CMSG_NETIF_FE_INTERFACE_STATUS_CHANGED, 0,
                        { 'handle' : 0, 'status' : 1 })
                    netif.ctrlif_tx_req(xend.main.port_from_dom(netif.dom),msg)
                print "Done notifying guests"
                recovery = False
        else:
            print "Unexpected net backend driver status: %d" % status

def backend_rx_rsp(port, msg):
    subtype = (msg.get_header())['subtype']
    print "Received netif-be response, subtype %d" % subtype
    if subtype == CMSG_NETIF_BE_CREATE:
        rsp = { 'success': True }
        xend.main.send_management_response(rsp, xend.netif.pendaddr)
    elif subtype == CMSG_NETIF_BE_CONNECT:
        pl = msg.get_payload()
        (dom, hnd, evtchn, tx_frame, rx_frame, st) = (
            pl['domid'], pl['netif_handle'], pl['evtchn'],
            pl['tx_shmem_frame'], pl['rx_shmem_frame'], pl['status'])
        netif = interface.list[xend.main.port_from_dom(dom).local_port]
        msg = xend.utils.message(CMSG_NETIF_FE,
                                 CMSG_NETIF_FE_INTERFACE_STATUS_CHANGED, 0,
                                 { 'handle' : 0, 'status' : 2,
                                   'evtchn' : netif.evtchn['port2'],
                                   'mac[0]' : netif.mac[0],
                                   'mac[1]' : netif.mac[1],
                                   'mac[2]' : netif.mac[2],
                                   'mac[3]' : netif.mac[3],
                                   'mac[4]' : netif.mac[4],
                                   'mac[5]' : netif.mac[5] })
        netif.ctrlif_tx_req(xend.main.port_list[netif.key], msg)

def backend_do_work(port):
    global pendmsg
    if pendmsg and port.space_to_write_request():
        port.write_request(pendmsg)
        pendmsg = None
        return True
    return False


class interface:

    # Dictionary of all network-device interfaces.
    list = {}

    drvdom = None

    # NB. 'key' is an opaque value that has no meaning in this class.
    def __init__(self, dom, key):
        self.dom     = dom
        self.key     = key
        self.pendmsg = None

        # VIFs get a random MAC address with a "special" vendor id.
        # 
        # NB. The vendor is currently an "obsolete" one that used to belong
        # to DEC (AA-00-00). Using it is probably a bit rude :-)
        # 
        # NB2. The first bit of the first random octet is set to zero for
        # all dynamic MAC addresses. This may allow us to manually specify
        # MAC addresses for some VIFs with no fear of clashes.
        self.mac = [ 0xaa, 0x00, 0x00 ]
        self.mac.append(int(random.random()*128))
        self.mac.append(int(random.random()*256))
        self.mac.append(int(random.random()*256))
                
        interface.list[key] = self
        self.create()

    def create(self):
        """Notify the current network back end to create the virtual interface
        represented by this object."""
        msg = xend.utils.message(CMSG_NETIF_BE, CMSG_NETIF_BE_CREATE, 0,
                                 { 'domid' : self.dom, 'netif_handle' : 0,
                                   'mac[0]' : self.mac[0],
                                   'mac[1]' : self.mac[1],
                                   'mac[2]' : self.mac[2],
                                   'mac[3]' : self.mac[3],
                                   'mac[4]' : self.mac[4],
                                   'mac[5]' : self.mac[5] })
        xend.netif.pendaddr = xend.main.mgmt_req_addr
        backend_tx_req(msg)


    # Completely destroy this interface.
    def destroy(self):
        del interface.list[self.key]
        msg = xend.utils.message(CMSG_NETIF_BE, CMSG_NETIF_BE_DESTROY, 0,
                                 { 'domid' : self.dom, 'netif_handle' : 0 })
        backend_tx_req(msg)        


    # The parameter @port is the control-interface event channel. This method
    # returns True if messages were written to the control interface.
    def ctrlif_transmit_work(self, port):
        if self.pendmsg and port.space_to_write_request():
            port.write_request(self.pendmsg)
            self.pendmsg = None
            return True
        return False

    def ctrlif_tx_req(self, port, msg):
        if port.space_to_write_request():
            port.write_request(msg)
            port.notify()
        else:
            self.pendmsg = msg

    def ctrlif_rx_req(self, port, msg):
        port.write_response(msg)
        subtype = (msg.get_header())['subtype']
        if subtype == CMSG_NETIF_FE_DRIVER_STATUS_CHANGED:
            print "netif driver up message from %d" % port.remote_dom
            msg = xend.utils.message(CMSG_NETIF_FE,
                                     CMSG_NETIF_FE_INTERFACE_STATUS_CHANGED, 0,
                                     { 'handle' : 0, 'status' : 1 })
            self.ctrlif_tx_req(port, msg)
        elif subtype == CMSG_NETIF_FE_INTERFACE_CONNECT:
            print "netif connect request from %d" % port.remote_dom
            pl = msg.get_payload()
            (hnd, tx_frame, rx_frame) = (pl['handle'], pl['tx_shmem_frame'],
                                         pl['rx_shmem_frame'])
            xc = Xc.new()
            self.evtchn = xc.evtchn_bind_interdomain(
                dom1=xend.netif.be_port.remote_dom,
                dom2=self.dom)
            msg = xend.utils.message(CMSG_NETIF_BE,
                                     CMSG_NETIF_BE_CONNECT, 0,
                                     { 'domid' : self.dom, 'netif_handle' : 0,
                                       'tx_shmem_frame' : tx_frame,
                                       'rx_shmem_frame' : rx_frame,
                                       'evtchn' : self.evtchn['port1'] })
            backend_tx_req(msg)
