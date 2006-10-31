#============================================================================
# This library is free software; you can redistribute it and/or
# modify it under the terms of version 2.1 of the GNU Lesser General Public
# License as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#============================================================================
# Copyright (C) 2004, 2005 Mike Wray <mike.wray@hp.com>
# Copyright (C) 2005, 2006 XenSource Inc.
#============================================================================


import re
import string

from xen.util import blkif
from xen.util import security
from xen.xend import sxp
from xen.xend.XendError import VmError

from xen.xend.server.DevController import DevController


class BlkifController(DevController):
    """Block device interface controller. Handles all block devices
    for a domain.
    """

    def __init__(self, vm):
        """Create a block device controller.
        """
        DevController.__init__(self, vm)


    def getDeviceDetails(self, config):
        """@see DevController.getDeviceDetails"""
        uname = sxp.child_value(config, 'uname')

        dev = sxp.child_value(config, 'dev')

        if 'ioemu:' in dev:
            (_, dev) = string.split(dev, ':', 1)
        try:
            (dev, dev_type) = string.split(dev, ':', 1)
        except ValueError:
            dev_type = "disk"

        if uname is None:
            if dev_type == 'cdrom':
                (typ, params) = ("", "")
            else:
                raise VmError(
                    'Block device must have physical details specified')
        else:
            try:
                (typ, params) = string.split(uname, ':', 1)
            except ValueError:
                (typ, params) = ("", "")

        mode = sxp.child_value(config, 'mode', 'r')
        if mode not in ('r', 'w', 'w!'):
            name = uname
            if not name:
                name = sxp.child_value(config, 'dev')
            raise VmError('Invalid mode \'%s\' for %s. Valid modes are '
            '{r,w,w!}' % (mode, name))

        back = { 'dev'    : dev,
                 'type'   : typ,
                 'params' : params,
                 'mode'   : mode
               }

        if security.on():
            (label, ssidref, policy) = security.get_res_security_details(uname)
            back.update({'acm_label'  : label,
                         'acm_ssidref': str(ssidref),
                         'acm_policy' : policy})

        devid = blkif.blkdev_name_to_number(dev)
        if not devid:
            raise VmError('Unable to find number for device (%s)' % (dev))

        front = { 'virtual-device' : "%i" % devid,
                  'device-type' : dev_type
                }

        return (devid, back, front)


    def reconfigureDevice(self, _, config):
        """@see DevController.reconfigureDevice"""
        (devid, new_back, new_front) = self.getDeviceDetails(config)

        (dev, mode) = self.readBackend(devid, 'dev', 'mode')
        dev_type = self.readFrontend(devid, 'device-type')

        if (dev_type == 'cdrom' and new_front['device-type'] == 'cdrom' and
            dev == new_back['dev'] and mode == 'r'):
            self.writeBackend(devid,
                              'type', new_back['type'],
                              'params', new_back['params'])
        else:
            raise VmError('Refusing to reconfigure device %s:%d to %s' %
                          (self.deviceClass, devid, config))


    def configuration(self, devid):
        """@see DevController.configuration"""

        result = DevController.configuration(self, devid)

        (dev, typ, params, mode) = self.readBackend(devid,
                                                    'dev', 'type', 'params',
                                                    'mode')

        if dev:
            (dev_type) = self.readFrontend(devid, 'device-type')
            if dev_type:
                dev += ":" + dev_type
            result.append(['dev', dev])
        if typ and params:
            result.append(['uname', typ + ":" + params])
        if mode:
            result.append(['mode', mode])

        return result


    def destroyDevice(self, devid):
        """@see DevController.destroyDevice"""

        # If we are given a device name, then look up the device ID from it,
        # and destroy that ID instead.  If what we are given is an integer,
        # then assume it's a device ID and pass it straight through to our
        # superclass's method.

        try:
            DevController.destroyDevice(self, int(devid))
        except ValueError:
            devid_end = type(devid) is str and devid.split('/')[-1] or None

            for i in self.deviceIDs():
                d = self.readBackend(i, 'dev')
                if d == devid or (devid_end and d == devid_end):
                    DevController.destroyDevice(self, i)
                    return
            raise VmError("Device %s not connected" % devid)
