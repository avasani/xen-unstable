from xen.xend.XendClient import server
from xen.xend import sxp
from xen.xend import PrettyPrint

import types

def getDomInfoHash( domain ):
    domInfoHash = sxp2hash( server.xend_domain( int( domain ) ) )
    domInfoHash['dom'] = int( domain )
    return domInfoHash

def sxp2hash( s ):
    sxphash = {}
        
    for child in sxp.children( s ):
    	if child is types.ListType:
            sxphash[ child[0] ] = sxp2hash( child[1] )
        else:
            sxphash[ child[0] ] = child[1]
        
    return sxphash
    
def sxp2string( sxp ):
    class tmp:
        def __init__( self ):
                self.str = ""
        def write( self, str ):
                self.str = self.str + str
    temp = tmp()
    PrettyPrint.prettyprint( sxp, out=temp )
    return temp.str

def getVar( var, request ):
   
    arg = request.args.get( var )

    if arg is None or len(arg) != 1:
        return None
    else:
        return arg[0]

def bigTimeFormatter( time ):
    time = float( time )
    weeks = time // 604800
    remainder = time % 604800
    days = remainder // 86400
    
    remainder = remainder % 86400

    hms = smallTimeFormatter( remainder )
    
    return "%d weeks, %d days, %s" % ( weeks, days, hms )

def smallTimeFormatter( time ):
    time = float( time )
    hours = time // 3600
    remainder = time % 3600
    mins = remainder // 60
    secs = time % 60
    return "%02d:%02d:%04.1f (hh:mm:ss.s)" % ( hours, mins, secs ) 

def stateFormatter( state ):
    states = [ 'Running', 'Blocked', 'Paused', 'Shutdown', 'Crashed' ]
    
    stateStr = ""
    
    for i in range( len( state ) ):
        if state[i] != "-":
            stateStr += "%s, " % states[ i ] 
           
    return stateStr + " (%s)" % state

def memoryFormatter( mem ):
    mem = int( mem )
    if mem >= 1024:
        mem = float( mem ) / 1024
        return "%3.2fGb" % mem
    else:    
        return "%7dMb" % mem

def cpuFormatter( mhz ):
    mhz = int( mhz )
    if mhz > 1000:
        ghz = float( mhz ) / 1000.0
        return "%4.2fGHz" % ghz
    else:
        return "%4dMHz" % mhz
        
def hyperthreadFormatter( threads ):
    if int( threads ) > 1:
        return "Yes (%d)" % threads
    else:
        return "No"
