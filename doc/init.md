Sample init scripts and service configuration for waggoxd
==========================================================

Sample scripts and configuration files for systemd, Upstart and OpenRC
can be found in the contrib/init folder.

    contrib/init/waggoxd.service:    systemd service unit configuration
    contrib/init/waggoxd.openrc:     OpenRC compatible SysV style init script
    contrib/init/waggoxd.openrcconf: OpenRC conf.d file
    contrib/init/waggoxd.conf:       Upstart service configuration file
    contrib/init/waggoxd.init:       CentOS compatible SysV style init script

1. Service User
---------------------------------

All three Linux startup configurations assume the existence of a "waggoxcore" user
and group.  They must be created before attempting to use these scripts.
The OS X configuration assumes waggoxd will be set up for the current user.

2. Configuration
---------------------------------

At a bare minimum, waggoxd requires that the rpcpassword setting be set
when running as a daemon.  If the configuration file does not exist or this
setting is not set, waggoxd will shutdown promptly after startup.

This password does not have to be remembered or typed as it is mostly used
as a fixed token that waggoxd and client programs read from the configuration
file, however it is recommended that a strong and secure password be used
as this password is security critical to securing the wallet should the
wallet be enabled.

If waggoxd is run with the "-server" flag (set by default), and no rpcpassword is set,
it will use a special cookie file for authentication. The cookie is generated with random
content when the daemon starts, and deleted when it exits. Read access to this file
controls who can access it through RPC.

By default the cookie is stored in the data directory, but it's location can be overridden
with the option '-rpccookiefile'.

This allows for running waggoxd without having to do any manual configuration.

`conf`, `pid`, and `wallet` accept relative paths which are interpreted as
relative to the data directory. `wallet` *only* supports relative paths.

For an example configuration file that describes the configuration settings,
see `contrib/debian/examples/waggox.conf`.

3. Paths
---------------------------------

3a) Linux

All three configurations assume several paths that might need to be adjusted.

Binary:              `/usr/bin/waggoxd`  
Configuration file:  `/etc/waggoxcore/waggox.conf`  
Data directory:      `/var/lib/waggoxd`  
PID file:            `/var/run/waggoxd/waggoxd.pid` (OpenRC and Upstart) or `/var/lib/waggoxd/waggoxd.pid` (systemd)  
Lock file:           `/var/lock/subsys/waggoxd` (CentOS)  

The configuration file, PID directory (if applicable) and data directory
should all be owned by the waggoxcore user and group.  It is advised for security
reasons to make the configuration file and data directory only readable by the
waggoxcore user and group.  Access to waggox-cli and other waggoxd rpc clients
can then be controlled by group membership.

3b) Mac OS X

Binary:              `/usr/local/bin/waggoxd`  
Configuration file:  `~/Library/Application Support/WaggoxCore/waggox.conf`  
Data directory:      `~/Library/Application Support/WaggoxCore`
Lock file:           `~/Library/Application Support/WaggoxCore/.lock`

4. Installing Service Configuration
-----------------------------------

4a) systemd

Installing this .service file consists of just copying it to
/usr/lib/systemd/system directory, followed by the command
`systemctl daemon-reload` in order to update running systemd configuration.

To test, run `systemctl start waggoxd` and to enable for system startup run
`systemctl enable waggoxd`

4b) OpenRC

Rename waggoxd.openrc to waggoxd and drop it in /etc/init.d.  Double
check ownership and permissions and make it executable.  Test it with
`/etc/init.d/waggoxd start` and configure it to run on startup with
`rc-update add waggoxd`

4c) Upstart (for Debian/Ubuntu based distributions)

Drop waggoxd.conf in /etc/init.  Test by running `service waggoxd start`
it will automatically start on reboot.

NOTE: This script is incompatible with CentOS 5 and Amazon Linux 2014 as they
use old versions of Upstart and do not supply the start-stop-daemon utility.

4d) CentOS

Copy waggoxd.init to /etc/init.d/waggoxd. Test by running `service waggoxd start`.

Using this script, you can adjust the path and flags to the waggoxd program by
setting the WAGGOXD and FLAGS environment variables in the file
/etc/sysconfig/waggoxd. You can also use the DAEMONOPTS environment variable here.

4e) Mac OS X

Copy org.waggox.waggoxd.plist into ~/Library/LaunchAgents. Load the launch agent by
running `launchctl load ~/Library/LaunchAgents/org.waggox.waggoxd.plist`.

This Launch Agent will cause waggoxd to start whenever the user logs in.

NOTE: This approach is intended for those wanting to run waggoxd as the current user.
You will need to modify org.waggox.waggoxd.plist if you intend to use it as a
Launch Daemon with a dedicated waggoxcore user.

5. Auto-respawn
-----------------------------------

Auto respawning is currently only configured for Upstart and systemd.
Reasonable defaults have been chosen but YMMV.
