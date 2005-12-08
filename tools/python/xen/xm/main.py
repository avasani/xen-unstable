# (C) Copyright IBM Corp. 2005
# Copyright (C) 2004 Mike Wray
# Copyright (c) 2005 XenSource Ltd
#
# Authors:
#     Sean Dague <sean at dague dot net>
#     Mike Wray <mike dot wray at hp dot com>
#
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

"""Grand unified management application for Xen.
"""
import os
import os.path
import sys
import re
import getopt
import socket
import warnings
warnings.filterwarnings('ignore', category=FutureWarning)

import xen.xend.XendError
import xen.xend.XendProtocol

from xen.xend import PrettyPrint
from xen.xend import sxp
from xen.xm.opts import *

import console


# getopt.gnu_getopt is better, but only exists in Python 2.3+.  Use
# getopt.getopt if gnu_getopt is not available.  This will mean that options
# may only be specified before positional arguments.
if not hasattr(getopt, 'gnu_getopt'):
    getopt.gnu_getopt = getopt.getopt


# Strings for shorthelp
console_help = "console <DomId>                  Attach to domain DomId's console."
create_help =  """create [-c] <ConfigFile>
               [Name=Value]..       Create a domain based on Config File"""
destroy_help = "destroy <DomId>                  Terminate a domain immediately"
help_help =    "help                             Display this message"
list_help =    "list [--long] [DomId, ...]       List information about domains"
mem_max_help = "mem-max <DomId> <Mem>            Set maximum memory reservation for a domain"
mem_set_help = "mem-set <DomId> <Mem>            Adjust the current memory usage for a domain"
migrate_help = "migrate <DomId> <Host>           Migrate a domain to another machine"
pause_help =   "pause <DomId>                    Pause execution of a domain"
reboot_help =  "reboot <DomId> [-w][-a]          Reboot a domain"
restore_help = "restore <File>                   Create a domain from a saved state file"
save_help =    "save <DomId> <File>              Save domain state (and config) to file"
shutdown_help ="shutdown <DomId> [-w][-a][-R|-H] Shutdown a domain"
top_help =     "top                              Monitor system and domains in real-time"
unpause_help = "unpause <DomId>                  Unpause a paused domain"

help_spacer = """
   """

# Strings for longhelp
sysrq_help =   "sysrq   <DomId> <letter>         Send a sysrq to a domain"
domid_help =   "domid <DomName>                  Converts a domain name to a domain id"
domname_help = "domname <DomId>                  Convert a domain id to a domain name"
vcpu_set_help = """vcpu-set <DomId> <VCPUs>         Set the number of VCPUs for a domain"""
vcpu_list_help = "vcpu-list <DomId>                List the VCPUs for a domain (or all domains)"
vcpu_pin_help = "vcpu-pin <DomId> <VCPU> <CPUs>   Set which cpus a VCPU can use" 
dmesg_help =   "dmesg [--clear]                  Read or clear Xen's message buffer"
info_help =    "info                             Get information about the xen host"
rename_help =  "rename <DomId> <New Name>        Rename a domain"
log_help =     "log                              Print the xend log"
sched_bvt_help = """sched-bvt <Parameters>           Set Borrowed Virtual Time scheduler
                                    parameters"""
sched_bvt_ctxallow_help = """sched-bvt-ctxallow <Allow>       Set the BVT scheduler context switch
                                    allowance"""
sched_sedf_help = "sched-sedf <Parameters>          Set simple EDF parameters"
block_attach_help = """block-attach <DomId> <BackDev> <FrontDev> <Mode>
                [BackDomId]         Create a new virtual block device"""
block_detach_help = """block-detach  <DomId> <DevId>    Destroy a domain's virtual block device,
                                    where <DevId> may either be the device ID
                                    or the device name as mounted in the guest"""

block_list_help = "block-list <DomId>               List virtual block devices for a domain"
network_attach_help = """network-attach  <DomID> [script=<script>] [ip=<ip>] [mac=<mac>]
                           [bridge=<bridge>] [backend=<backDomID>]
                                    Create a new virtual network device """
network_detach_help = """network-detach  <DomId> <DevId>  Destroy a domain's virtual network
                                    device, where <DevId> is the device ID."""

network_list_help = "network-list <DomId>             List virtual network interfaces for a domain"
vnet_list_help = "vnet-list [-l|--long]            list vnets"
vnet_create_help = "vnet-create <config>             create a vnet from a config file"
vnet_delete_help = "vnet-delete <vnetid>             delete a vnet"


short_command_list = [
    "console",
    "create",
    "destroy",
    "help",
    "list",
    "mem-set",
    "migrate",
    "pause",
    "reboot",
    "restore",
    "save",
    "shutdown",
    "top",
    "unpause",
    "vcpu-set",
    ]

domain_commands = [
    "console",
    "create",
    "destroy",
    "domid",
    "domname",
    "list",
    "mem-max",
    "mem-set",
    "migrate",
    "pause",
    "reboot",
    "rename",
    "restore",
    "save",
    "shutdown",
    "sysrq",
    "top",
    "unpause",
    "vcpu-list",
    "vcpu-pin",
    "vcpu-set",
    ]

host_commands = [
    "dmesg",
    "info",
    "log",
    "top",
    ]

scheduler_commands = [
    "sched-bvt",
    "sched-bvt-ctxallow",
    "sched-sedf",
    ]

device_commands = [
    "block-attach",
    "block-detach",
    "block-list",
    "network-attach",
    "network-detach",
    "network-list",
    ]

vnet_commands = [
    "vnet-list",
    "vnet-create",
    "vnet-delete",
    ]

all_commands = (domain_commands + host_commands + scheduler_commands +
                device_commands + vnet_commands)


def commandToHelp(cmd):
    return eval(cmd.replace("-", "_") + "_help")


shorthelp = """Usage: xm <subcommand> [args]
    Control, list, and manipulate Xen guest instances

xm common subcommands:
   """  + help_spacer.join(map(commandToHelp, short_command_list))  + """

<DomName> can be substituted for <DomId> in xm subcommands.

For a complete list of subcommands run 'xm help --long'
For more help on xm see the xm(1) man page
For more help on xm create, see the xmdomain.cfg(5) man page"""

longhelp = """Usage: xm <subcommand> [args]
    Control, list, and manipulate Xen guest instances

xm full list of subcommands:

  Domain Commands:
   """ + help_spacer.join(map(commandToHelp,  domain_commands)) + """

  Xen Host Commands:
   """ + help_spacer.join(map(commandToHelp,  host_commands)) + """

  Scheduler Commands:
   """ + help_spacer.join(map(commandToHelp,  scheduler_commands)) + """

  Virtual Device Commands:
   """  + help_spacer.join(map(commandToHelp, device_commands)) + """

  Vnet commands:
   """ + help_spacer.join(map(commandToHelp,  vnet_commands)) + """

<DomName> can be substituted for <DomId> in xm subcommands.

For a short list of subcommands run 'xm help'
For more help on xm see the xm(1) man page
For more help on xm create, see the xmdomain.cfg(5) man page"""

# array for xm help <command>
help = {
    "--long": longhelp
    }

for command in all_commands:
    # create is handled specially
    if (command != 'create'):
        help[command] = commandToHelp(command)


####################################################################
#
#  Utility functions
#
####################################################################

def arg_check(args, name, lo, hi = -1):
    n = len(args)
    
    if hi == -1:
        if n != lo:
            err("'xm %s' requires %d argument%s.\n" % (name, lo,
                                                       lo > 1 and 's' or ''))
            usage(name)
    else:
        if n < lo or n > hi:
            err("'xm %s' requires between %d and %d arguments.\n" %
                (name, lo, hi))
            usage(name)


def unit(c):
    if not c.isalpha():
        return 0
    base = 1
    if c == 'G' or c == 'g': base = 1024 * 1024 * 1024
    elif c == 'M' or c == 'm': base = 1024 * 1024
    elif c == 'K' or c == 'k': base = 1024
    else:
        print 'ignoring unknown unit'
    return base

def int_unit(str, dest):
    base = unit(str[-1])
    if not base:
        return int(str)

    value = int(str[:-1])
    dst_base = unit(dest)
    if dst_base == 0:
        dst_base = 1
    if dst_base > base:
        return value / (dst_base / base)
    else:
        return value * (base / dst_base)

def err(msg):
    print >>sys.stderr, "Error:", msg

def handle_xend_error(cmd, args, ex):
    non_option = filter(lambda x: x[0] != '-', args)
    dom = len(non_option) > 0 and non_option[0] or None

    error = str(ex)
    if error == "Not found" and dom != None:
        err("Domain '%s' not found when running 'xm %s'" % (dom, cmd))
    else:
        err(error)

    sys.exit(1)
    

#########################################################################
#
#  Main xm functions
#
#########################################################################

def xm_save(args):
    arg_check(args, "save", 2)

    dom = args[0] # TODO: should check if this exists
    savefile = os.path.abspath(args[1])

    if not os.access(os.path.dirname(savefile), os.W_OK):
        err("xm save: Unable to create file %s" % savefile)
        sys.exit(1)
    
    from xen.xend.XendClient import server
    server.xend_domain_save(dom, savefile)
    
def xm_restore(args):
    arg_check(args, "restore", 1)

    savefile = os.path.abspath(args[0])

    if not os.access(savefile, os.R_OK):
        err("xm restore: Unable to read file %s" % savefile)
        sys.exit(1)

    from xen.xend.XendClient import server
    server.xend_domain_restore(savefile)


def getDomains(domain_names):
    from xen.xend.XendClient import server
    if domain_names:
        return map(server.xend_domain, domain_names)
    else:
        return server.xend_list_domains()


def xm_list(args):
    use_long = 0
    show_vcpus = 0
    try:
        (options, params) = getopt.gnu_getopt(args, 'lv', ['long','vcpus'])
    except getopt.GetoptError, opterr:
        err(opterr)
        sys.exit(1)
    
    for (k, v) in options:
        if k in ['-l', '--long']:
            use_long = 1
        if k in ['-v', '--vcpus']:
            show_vcpus = 1

    if show_vcpus:
        print >>sys.stderr, (
            "xm list -v is deprecated.  Please use xm vcpu-list.")
        xm_vcpu_list(params)
        return

    doms = getDomains(params)

    if use_long:
        map(PrettyPrint.prettyprint, doms)
    else:
        xm_brief_list(doms)


def parse_doms_info(info):
    def get_info(n, t, d):
        return t(sxp.child_value(info, n, d))
    
    return {
        'dom'      : get_info('domid',        int,   -1),
        'name'     : get_info('name',         str,   '??'),
        'mem'      : get_info('memory',       int,   0),
        'vcpus'    : get_info('online_vcpus', int,   0),
        'state'    : get_info('state',        str,   '??'),
        'cpu_time' : get_info('cpu_time',     float, 0),
        'ssidref'  : get_info('ssidref',      int,   0),
        }


def xm_brief_list(doms):
    print 'Name                              ID Mem(MiB) VCPUs State  Time(s)'
    for dom in doms:
        d = parse_doms_info(dom)
        if (d['ssidref'] != 0):
            d['ssidstr'] = (" s:%04x/p:%04x" % 
                            ((d['ssidref'] >> 16) & 0xffff,
                              d['ssidref']        & 0xffff))
        else:
            d['ssidstr'] = ""
        print ("%(name)-32s %(dom)3d %(mem)8d %(vcpus)5d %(state)5s %(cpu_time)7.1f%(ssidstr)s" % d)


def xm_vcpu_list(args):
    print 'Name                              ID  VCPU  CPU  State  Time(s)  CPU Affinity'

    from xen.xend.XendClient import server
    if args:
        dominfo = map(server.xend_domain_vcpuinfo, args)
    else:
        doms = server.xend_list_domains()
        dominfo = map(
            lambda x: server.xend_domain_vcpuinfo(sxp.child_value(x, 'name')),
            doms)

    for dom in dominfo:
        def get_info(n):
            return sxp.child_value(dom, n)

        #
        # convert a list of integers into a list of pairs indicating
        # continuous sequences in the list:
        #
        # [0,1,2,3]   -> [(0,3)]
        # [1,2,4,5]   -> [(1,2),(4,5)]
        # [0]         -> [(0,0)]
        # [0,1,4,6,7] -> [(0,1),(4,4),(6,7)]
        #
        def list_to_rangepairs(cmap):
            cmap.sort()
            pairs = []
            x = y = 0
            for i in range(0,len(cmap)):
                try:
                    if ((cmap[y+1] - cmap[i]) > 1):
                        pairs.append((cmap[x],cmap[y]))
                        x = y = i+1
                    else:
                        y = y + 1
                # if we go off the end, then just add x to y
                except IndexError:
                    pairs.append((cmap[x],cmap[y]))

            return pairs

        #
        # Convert pairs to range string, e.g: [(1,2),(3,3),(5,7)] -> 1-2,3,5-7
        #
        def format_pairs(pairs):
            if not pairs:
                return "no cpus"
            out = ""
            for f,s in pairs:
                if (f==s):
                    out += '%d'%f
                else:
                    out += '%d-%d'%(f,s)
                out += ','
            # trim trailing ','
            return out[:-1]

        def format_cpumap(cpumap):
            cpumap = map(lambda x: int(x), cpumap)
            cpumap.sort()

            from xen.xend.XendClient import server
            for x in server.xend_node()[1:]:
                if len(x) > 1 and x[0] == 'nr_cpus':
                    nr_cpus = int(x[1])
                    # normalize cpumap by modulus nr_cpus, and drop duplicates
                    cpumap = dict.fromkeys(
                                map(lambda x: x % nr_cpus, cpumap)).keys()
                    if len(cpumap) == nr_cpus:
                        return "any cpu"
                    break
 
            return format_pairs(list_to_rangepairs(cpumap))

        name  =     get_info('name')
        domid = int(get_info('domid'))

        for vcpu in sxp.children(dom, 'vcpu'):
            def vinfo(n, t):
                return t(sxp.child_value(vcpu, n))

            number   = vinfo('number',   int)
            cpu      = vinfo('cpu',      int)
            cpumap   = format_cpumap(vinfo('cpumap', list))
            online   = vinfo('online',   int)
            cpu_time = vinfo('cpu_time', float)
            running  = vinfo('running',  int)
            blocked  = vinfo('blocked',  int)

            if online:
                c = str(cpu)
                if running:
                    s = 'r'
                else:
                    s = '-'
                if blocked:
                    s += 'b'
                else:
                    s += '-'
                s += '-'
            else:
                c = "-"
                s = "--p"

            print (
                "%(name)-32s %(domid)3d  %(number)4d  %(c)3s   %(s)-3s   %(cpu_time)7.1f  %(cpumap)s" %
                locals())


def xm_reboot(args):
    arg_check(args, "reboot", 1, 4)
    from xen.xm import shutdown
    shutdown.main(["shutdown", "-R"] + args)

def xm_pause(args):
    arg_check(args, "pause", 1)
    dom = args[0]

    from xen.xend.XendClient import server
    server.xend_domain_pause(dom)

def xm_unpause(args):
    arg_check(args, "unpause", 1)
    dom = args[0]

    from xen.xend.XendClient import server
    server.xend_domain_unpause(dom)

def xm_rename(args):
    arg_check(args, "rename", 2)

    from xen.xend.XendClient import server
    server.xend_domain_rename(args[0], args[1])

def xm_subcommand(command, args):
    cmd = __import__(command, globals(), locals(), 'xen.xm')
    cmd.main([command] + args)


#############################################################

def cpu_make_map(cpulist):
    cpus = []
    for c in cpulist.split(','):
        if c.find('-') != -1:
            (x,y) = c.split('-')
            for i in range(int(x),int(y)+1):
                cpus.append(int(i))
        else:
            cpus.append(int(c))
    cpus.sort()
    return cpus

def xm_vcpu_pin(args):
    arg_check(args, "vcpu-pin", 3)

    dom  = args[0]
    vcpu = int(args[1])
    cpumap = cpu_make_map(args[2])
    
    from xen.xend.XendClient import server
    server.xend_domain_pincpu(dom, vcpu, cpumap)

def xm_mem_max(args):
    arg_check(args, "mem-max", 2)

    dom = args[0]
    mem = int_unit(args[1], 'm')

    from xen.xend.XendClient import server
    server.xend_domain_maxmem_set(dom, mem)
    
def xm_mem_set(args):
    arg_check(args, "mem-set", 2)

    dom = args[0]
    mem_target = int_unit(args[1], 'm')

    from xen.xend.XendClient import server
    server.xend_domain_mem_target_set(dom, mem_target)
    
def xm_vcpu_set(args):
    arg_check(args, "vcpu-set", 2)
    
    from xen.xend.XendClient import server
    server.xend_domain_set_vcpus(args[0], int(args[1]))


def xm_destroy(args):
    arg_check(args, "destroy", 1)
    from xen.xend.XendClient import server
    server.xend_domain_destroy(args[0])


def xm_domid(args):
    arg_check(args, "domid", 1)

    name = args[0]

    from xen.xend.XendClient import server
    dom = server.xend_domain(name)
    print sxp.child_value(dom, 'domid')
    
def xm_domname(args):
    arg_check(args, "domname", 1)

    name = args[0]

    from xen.xend.XendClient import server
    dom = server.xend_domain(name)
    print sxp.child_value(dom, 'name')

def xm_sched_bvt(args):
    arg_check(args, "sched-bvt", 6)
    dom = args[0]
    v = map(long, args[1:6])
    from xen.xend.XendClient import server
    server.xend_domain_cpu_bvt_set(dom, *v)

def xm_sched_bvt_ctxallow(args):
    arg_check(args, "sched-bvt-ctxallow", 1)

    slice = int(args[0])
    from xen.xend.XendClient import server
    server.xend_node_cpu_bvt_slice_set(slice)

def xm_sched_sedf(args):
    arg_check(args, "sched-sedf", 6)
    
    dom = args[0]
    v = map(int, args[1:6])
    from xen.xend.XendClient import server
    server.xend_domain_cpu_sedf_set(dom, *v)

def xm_info(args):
    from xen.xend.XendClient import server
    info = server.xend_node()
    
    for x in info[1:]:
        if len(x) < 2: 
            print "%-23s: (none)" % x[0]
        else: 
            print "%-23s:" % x[0], x[1]

def xm_console(args):
    arg_check(args, "console", 1)

    dom = args[0]
    from xen.xend.XendClient import server
    info = server.xend_domain(dom)
    domid = int(sxp.child_value(info, 'domid', '-1'))
    console.execConsole(domid)


def xm_top(args):
    os.execvp('xentop', ['xentop'])

def xm_dmesg(args):
    
    gopts = Opts(use="""[-c|--clear]

Read Xen's message buffer (boot output, warning and error messages) or clear
its contents if the [-c|--clear] flag is specified.
""")

    gopts.opt('clear', short='c',
              fn=set_true, default=0,
              use="Clear the contents of the Xen message buffer.")
    # Work around for gopts
    myargs = args
    myargs.insert(0, 'dmesg')
    gopts.parse(myargs)
    if not (1 <= len(myargs) <= 2):
        err('Invalid arguments: ' + str(myargs))

    from xen.xend.XendClient import server
    if not gopts.vals.clear:
        print server.xend_node_get_dmesg()
    else:
        server.xend_node_clear_dmesg()

def xm_log(args):
    arg_check(args, 'xm-log', 0)
    
    from xen.xend.XendClient import server
    print server.xend_node_log()

def xm_network_list(args):
    arg_check(args, "network-list", 1)
    dom = args[0]
    from xen.xend.XendClient import server
    for x in server.xend_domain_devices(dom, 'vif'):
        sxp.show(x)
        print

def xm_block_list(args):
    arg_check(args, "block-list", 1)
    dom = args[0]
    from xen.xend.XendClient import server
    for x in server.xend_domain_devices(dom, 'vbd'):
        sxp.show(x)
        print

def xm_block_attach(args):
    arg_check(args, 'block-attach', 4, 5)

    dom = args[0]
    vbd = ['vbd',
           ['uname', args[1]],
           ['dev',   args[2]],
           ['mode',  args[3]]]
    if len(args) == 5:
        vbd.append(['backend', args[4]])

    from xen.xend.XendClient import server
    server.xend_domain_device_create(dom, vbd)


def xm_network_attach(args):
    arg_check(args, 'network-attach', 1, 10000)

    dom = args[0]
    vif = ['vif']

    for a in args[1:]:
        vif.append(a.split("="))

    from xen.xend.XendClient import server
    server.xend_domain_device_create(dom, vif)


def detach(args, command, deviceClass):
    arg_check(args, command, 2)

    dom = args[0]
    dev = args[1]

    from xen.xend.XendClient import server
    server.xend_domain_device_destroy(dom, deviceClass, dev)


def xm_block_detach(args):
    detach(args, 'block-detach', 'vbd')


def xm_network_detach(args):
    detach(args, 'network-detach', 'vif')


def xm_vnet_list(args):
    from xen.xend.XendClient import server
    try:
        (options, params) = getopt.gnu_getopt(args, 'l', ['long'])
    except getopt.GetoptError, opterr:
        err(opterr)
        sys.exit(1)
    
    use_long = 0
    for (k, v) in options:
        if k in ['-l', '--long']:
            use_long = 1
            
    if params:
        use_long = 1
        vnets = params
    else:
        vnets = server.xend_vnets()
    
    for vnet in vnets:
        try:
            if use_long:
                info = server.xend_vnet(vnet)
                PrettyPrint.prettyprint(info)
            else:
                print vnet
        except Exception, ex:
            print vnet, ex

def xm_vnet_create(args):
    arg_check(args, "vnet-create", 1)
    conf = args[0]
    if not os.access(conf, os.R_OK):
        print "File not found: %s" % conf
        sys.exit(1)

    from xen.xend.XendClient import server
    server.xend_vnet_create(conf)

def xm_vnet_delete(args):
    arg_check(args, "vnet-delete", 1)
    vnet = args[0]
    from xen.xend.XendClient import server
    server.xend_vnet_delete(vnet)

commands = {
    # console commands
    "console": xm_console,
    # xenstat commands
    "top": xm_top,
    # domain commands
    "destroy": xm_destroy,
    "domid": xm_domid,
    "domname": xm_domname,
    "rename": xm_rename,
    "restore": xm_restore,
    "save": xm_save,
    "reboot": xm_reboot,
    "list": xm_list,
    # memory commands
    "mem-max": xm_mem_max,
    "mem-set": xm_mem_set,
    # cpu commands
    "vcpu-pin": xm_vcpu_pin,
    "vcpu-list": xm_vcpu_list,
    "vcpu-set": xm_vcpu_set,
    # special
    "pause": xm_pause,
    "unpause": xm_unpause,
    # host commands
    "dmesg": xm_dmesg,
    "info": xm_info,
    "log": xm_log,
    # scheduler
    "sched-bvt": xm_sched_bvt,
    "sched-bvt-ctxallow": xm_sched_bvt_ctxallow,
    "sched-sedf": xm_sched_sedf,
    # block
    "block-attach": xm_block_attach,
    "block-detach": xm_block_detach,
    "block-list": xm_block_list,
    # network
    "network-attach": xm_network_attach,
    "network-detach": xm_network_detach,
    "network-list": xm_network_list,
    # vnet
    "vnet-list": xm_vnet_list,
    "vnet-create": xm_vnet_create,
    "vnet-delete": xm_vnet_delete,
    }

## The commands supported by a separate argument parser in xend.xm.
subcommands = [
    'create',
    'migrate',
    'sysrq',
    'shutdown'
    ]

for c in subcommands:
    commands[c] = eval('lambda args: xm_subcommand("%s", args)' % c)

aliases = {
    "balloon": "mem-set",
    "set-vcpus": "vcpu-set",
    "vif-list": "network-list",
    "vbd-create": "block-create",
    "vbd-destroy": "block-destroy",
    "vbd-list": "block-list",
    }


def xm_lookup_cmd(cmd):
    if commands.has_key(cmd):
        return commands[cmd]
    elif aliases.has_key(cmd):
        deprecated(cmd,aliases[cmd])
        return commands[aliases[cmd]]
    else:
        if len( cmd ) > 1:
            matched_commands = filter( lambda (command, func): command[ 0:len(cmd) ] == cmd, commands.iteritems() )
            if len( matched_commands ) == 1:
		        return matched_commands[0][1]
        err('Sub Command %s not found!' % cmd)
        usage()

def deprecated(old,new):
    print >>sys.stderr, (
        "Command %s is deprecated.  Please use xm %s instead." % (old, new))

def usage(cmd=None):
    if cmd == 'create':
        mycmd = xm_lookup_cmd(cmd)
        mycmd( ['--help'] )
        sys.exit(1)
    if help.has_key(cmd):
        print "   " + help[cmd]
    else:
        print shorthelp
    sys.exit(1)

def main(argv=sys.argv):
    if len(argv) < 2:
        usage()
    
    if re.compile('-*help').match(argv[1]):
	if len(argv) > 2:
	    usage(argv[2])
	else:
	    usage()
	sys.exit(0)

    cmd = xm_lookup_cmd(argv[1])

    # strip off prog name and subcmd
    args = argv[2:]
    if cmd:
        try:
            rc = cmd(args)
            if rc:
                usage()
        except socket.error, ex:
            if os.geteuid() != 0:
                err("Most commands need root access.  Please try again as root.")
            else:
                err("Error connecting to xend: %s.  Is xend running?" % ex[1])
            sys.exit(1)
        except KeyboardInterrupt:
            print "Interrupted."
            sys.exit(1)
        except IOError, ex:
            if os.geteuid() != 0:
                err("Most commands need root access.  Please try again as root.")
            else:
                err("Error connecting to xend: %s." % ex[1])
            sys.exit(1)
        except xen.xend.XendError.XendError, ex:
            if len(args) > 0:
                handle_xend_error(argv[1], args, ex)
            else:
                print "Unexpected error:", sys.exc_info()[0]
                print
                print "Please report to xen-devel@lists.xensource.com"
                raise
        except xen.xend.XendProtocol.XendError, ex:
            if len(args) > 0:
                handle_xend_error(argv[1], args, ex)
            else:
                print "Unexpected error:", sys.exc_info()[0]
                print
                print "Please report to xen-devel@lists.xensource.com"
                raise
        except SystemExit:
            sys.exit(1)
        except:
            print "Unexpected error:", sys.exc_info()[0]
            print
            print "Please report to xen-devel@lists.xensource.com"
            raise
                
    else:
        usage()

if __name__ == "__main__":
    main()
