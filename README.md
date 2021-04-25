# What is this?

This is Doom VNC, a patch on top of Chocolate Doom which replaces the native AV
and input support via SDL with a VNC server, so that you can connect to and play
Doom over a network without having to have the game available locally.

# Why?

I've been looking to do something with the VNC protocol for a while after coming
across RFC 6143. Since Doom uses software rendering and has an IO system that's
fairly easy to swap out, it's a good fit for doing something weird like this.

# Is it playable?

Yes! Well, assuming you can get it to build - I've only tested this on Linux
x64, although the socket APIs should work reasonably well across anything
Unix-like. When you launch the chocolate-doom executable it will bind to the
second VNC display (port 5902). As long as you have a VNC viewer you can connect
and play it.

# What are the bandwidth requirements?

It depends on the connection mode your VNC client is using. Most of them will
have at least two - a "high quality" and "low quality" mode. By default they
will use high quality when running from localhost, for remote machines it depends
on your settings.

Here "high quality" is the VNC RAW encoding, which essentially dumps out Doom's
framebuffer onto the network for each redraw request. Even at only 320x200,
that's about 1.5 MB/s if you're playing at a decent frame rate. "Low quality" is
the Tight encoding, which is more compact and uses about 100 KB/s.

If you're interested in the details, I encourage you to read SendRawVNCFrame and
SendTightVNCFrame in i_vnc.c. Tight is by far the more efficient encoding because
we can use Doom's native palettes with it instead of 32-bit colors, but it's also
zlib based and I had to add support for creating zlib frames without any underlying 
compression (RFC 1950 and 1951 are your friends here).

# Ugh! Why does only <some key> not work?

Blame RFC 6143. It exposes keys in the form of keysyms instead of the scancodes
that Doom is used to ingesting, so I had to add some mapping to get from the one
format to the other. It's entirely possible I missed a mapping.

----

# Chocolate Doom

Chocolate Doom aims to accurately reproduce the original DOS version of
Doom and other games based on the Doom engine in a form that can be
run on modern computers.

Originally, Chocolate Doom was only a Doom source port. The project
now includes ports of Heretic and Hexen, and Strife.

Chocolate Doom’s aims are:

 * To always be 100% Free and Open Source software.
 * Portability to as many different operating systems as possible.
 * Accurate reproduction of the original DOS versions of the games,
   including bugs.
 * Compatibility with the DOS demo, configuration and savegame files.
 * To provide an accurate retro “feel” (display and input should
   behave the same).

More information about the philosophy and design behind Chocolate Doom
can be found in the PHILOSOPHY file distributed with the source code.

## Setting up gameplay

For instructions on how to set up Chocolate Doom for play, see the
INSTALL file.

## Configuration File

Chocolate Doom is compatible with the DOS Doom configuration file
(normally named `default.cfg`). Existing configuration files for DOS
Doom should therefore simply work out of the box. However, Chocolate
Doom also provides some extra settings. These are stored in a
separate file named `chocolate-doom.cfg`.

The configuration can be edited using the chocolate-setup tool.

## Command line options

Chocolate Doom supports a number of command line parameters, including
some extras that were not originally suported by the DOS versions. For
binary distributions, see the CMDLINE file included with your
download; more information is also available on the Chocolate Doom
website.

## Playing TCs

With Vanilla Doom there is no way to include sprites in PWAD files.
Chocolate Doom’s ‘-file’ command line option behaves exactly the same
as Vanilla Doom, and trying to play TCs by adding the WAD files using
‘-file’ will not work.

Many Total Conversions (TCs) are distributed as a PWAD file which must
be merged into the main IWAD. Typically a copy of DEUSF.EXE is
included which performs this merge. Chocolate Doom includes a new
option, ‘-merge’, which will simulate this merge. Essentially, the
WAD directory is merged in memory, removing the need to modify the
IWAD on disk.

To play TCs using Chocolate Doom, run like this:

```
chocolate-doom -merge thetc.wad
```

Here are some examples:

```
chocolate-doom -merge batman.wad -deh batman.deh vbatman.deh  (Batman Doom)
chocolate-doom -merge aoddoom1.wad -deh aoddoom1.deh  (Army of Darkness Doom)
```

## Other information

 * Chocolate Doom includes a number of different options for music
   playback. See the README.Music file for more details.

 * More information, including information about how to play various
   classic TCs, is available on the Chocolate Doom website:

     https://www.chocolate-doom.org/

   You are encouraged to sign up and contribute any useful information
   you may have regarding the port!

 * Chocolate Doom is not perfect. Although it aims to accurately
   emulate and reproduce the DOS executables, some behavior can be very
   difficult to reproduce. Because of the nature of the project, you
   may also encounter Vanilla Doom bugs; these are intentionally
   present; see the NOT-BUGS file for more information.

   New bug reports can be submitted to the issue tracker on Github:

     https://github.com/chocolate-doom/chocolate-doom/issues

 * Source code patches are welcome, but please follow the style
   guidelines - see the file named HACKING included with the source
   distribution.

 * Chocolate Doom is distributed under the GNU GPL. See the COPYING
   file for more information.

 * Please send any feedback, questions or suggestions to
   chocolate-doom-dev-list@chocolate-doom.org. Thanks!
