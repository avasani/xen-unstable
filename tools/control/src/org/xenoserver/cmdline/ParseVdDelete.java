package org.xenoserver.cmdline;

import java.util.LinkedList;

import org.xenoserver.control.CommandFailedException;
import org.xenoserver.control.CommandVdDelete;
import org.xenoserver.control.Defaults;

public class ParseVdDelete extends CommandParser {
    public void parse(Defaults d, LinkedList args)
        throws ParseFailedException, CommandFailedException {
        String vd_key = getStringParameter(args, 'k', "");
        boolean force = getFlagParameter(args,'f');

        if (vd_key.equals("")) {
            throw new ParseFailedException("Expected -k<key>");
        }

        loadState();
        String output = new CommandVdDelete(vd_key,force).execute();
        if (output != null) {
            System.out.println(output);
        }

        saveState();
    }

    public String getName() {
        return "delete";
    }

    public String getUsage() {
        return "-k<key> [-f]";
    }

    public String getHelpText() {
        return "Deletes the virtual disk with the specified key. -f forces deletion even if the disk is in use.";
    }

}
