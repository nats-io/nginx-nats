nginx-nats
==========

Nginx module that implements NATS client.

#### Configuration:

NATS configuration is a section specified at the main level (i.e. not inside the `http` section).

    nats {
        server host1:port1;
        ...
        server hostN:portN;

        reconnect 2s;
        ping      30s;

        user      <username>;
        password  <password>;
    }
    
    http {
        ....

* One or more NATS servers can be specified. Nginx tries in listed order and connects to first available.
* If connection cannot be created or NATS disconnects Nginx tries all listed servers
  then waits for specified `reconnect` interval before it tries to connect again.
  Default reeconnect interval is 1 second.
* `ping` specified the interval at which Nginx sends __PING__ messages to NATS server.
   Default is 30 seconds.
* `user` and `password` are required if NATS server is configured to require authentication.
   Currently it applies to all servers, user/password specific to each server may be added in future.

#### Build:

Usually this module is built with other modules that use it because it does not do anything by itself,
except connectiong to NATS server. See instructions for those modules.


