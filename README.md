nginx-nats
==========

[![License Apache 2.0](https://img.shields.io/badge/License-Apache2-blue.svg)](https://www.apache.org/licenses/LICENSE-2.0)

Nginx module that implements the NATS client.

### Configuration:

NATS configuration is a section specified at the main level (i.e. not inside
the `http` section).

```nginx
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
    }
```

* One or more NATS servers can be specified. Nginx tries them in the listed
  order and connects to first available ("first working", not "round-robin" or
  other load-balancing).
* If a connection cannot be created or NATS disconnects then Nginx tries all
  listed servers then waits for specified `reconnect` interval before it tries
  to connect again.  Default reconnect interval is 1 second.
* `ping` specified the interval at which Nginx sends __PING__ messages to NATS
  server.  Default is 30 seconds.
* `user` and `password` are required if the NATS server has been configured to
  require authentication.  Currently it applies to all servers; per-server
  user/password authentication is a possible future feature.

### Build:

This module only maintains connections to NATS but does not do anything with
those connections, so usually this module should be built together with some
other module using the API which we export.

Like any nginx module, building nginx-nats is handled by configuring an nginx
build to reference this module, then building nginx.

This module depends upon:

* OpenSSL (currently just for randomness, but in the future for secured
  connections)

```console
nginx-src-dir$ ./configure [...] \
  --add-module=/path/to/github.com/apcera/nginx-nats/src \
  --add-module=/path/to/module/which/uses/nats \
  --with-cc-opt=-I${OPENSSLDIR:?}/include \
  "--with-ld-opt=-L${OPENSSLDIR:?}/lib -lssl -lcrypto [...]"
nginx-src-dir$ make
```

### API

There are two available header files

* `ngx_nats.h` -- core of the features
* `ngx_nats_json.h` -- interacting with NATS via JSON objects

All JSON methods, types and macro constants begin `ngx_nats_json` (or the
upper-case version of that).

#### ngx\_nats.h

The core type is `ngx_nats_client_t`, which is a struct type, thus variables
will be of type `ngx_nats_client_t *`.  The contents are outlined below, after
the callback functions.

The outline of the API is below, see the header for parameters and types.

Basic usage:

* `ngx_nats_add_client()` -- register a client which is using nginx-nats
* `ngx_nats_publish()` -- publish a message, serialized; optionally in reply
  to another message
* `ngx_nats_subscribe()` -- subscribe to receive messages
* `ngx_nats_unsubscribe()` -- end a subscription
* `ngx_nats_create_inbox()` -- create an end-point to receive a reply to a
  message

There are three callback functions which are needed:

* `ngx_nats_connected_pt` -- as `(ngx_nats_client_t*).connected`
* `ngx_nats_disconnected_pt` -- as `(ngx_nats_client_t*).disconnected`
* `ngx_nats_handle_msg_pt` -- passed as parameter to `ngx_nats_subscribe()`

A `ngx_nats_client_t *client` contains three members, which should be set by
the caller:

* `.connected` -- callback function
* `.disconnected` -- callback function
* `.data` -- a `void *` callback data blob for use by the caller

Utilities:

* `ngx_nats_init_random()` -- initialise randomness in the nginx-nats module
* `ngx_nats_next_random()` → `uint32_t`: get 32-bits of randomness
* `ngx_nats_get_local_ip()` → `ngx_addr_t *` (or `NULL`) -- our local IP

#### ngx\_nats\_json.h

JSON structure types: `ngx_nats_json_object_t`, `ngx_nats_json_array_t`,
`ngx_nats_json_value_t`, `ngx_nats_json_field_t`.

* `ngx_nats_json_parse()`: parse a serialized message into JSON structure
  types, using the provided allocation pool
* `ngx_nats_json_type_name()`: convert an `NGX_NATS_JSON_*` type constant into
  a string name
  
## License

Unless otherwise noted, the NATS source files are distributed under
the Apache Version 2.0 license found in the LICENSE file.
