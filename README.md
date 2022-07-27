AGL Application Launcher service reference implementation

`applaunchd` is a simple service for launching applications from other
applications. It exposes an interface named 'org.automotivelinux.AppLaunch' on
on the D-Bus session bus and can be autostarted by using this interface name.

This interface can be used to:
- retrieve a list of available applications
- request that a specific application be started by using the 'start' method
- subcribe to the 'started' and/or 'terminated' signals in order to be
  notified when an application started successfully or terminated

For more details about the D-Bus interface, please refer to the file
`data/org.automotivelinux.AppLaunch.xml`.

Applications are enumerated from systemd's list of available units based on
the pattern agl-app*@*.service, and are started and controled using their
systemd unit.  Please note `applaunchd` allows only one instance of a given
application.

AGL repo for source code:
https://gerrit.automotivelinux.org/gerrit/#/admin/projects/src/applaunchd

You can also clone the source repository by running the following command:
```
$ git clone https://gerrit.automotivelinux.org/gerrit/src/applaunchd
```
