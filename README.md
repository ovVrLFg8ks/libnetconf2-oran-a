**libnetconf2 fork** 


## Requirements

* C compiler (gcc >= 4.8.4, clang >= 3.0, ...)
* cmake >= 3.5.0
* crypt(3)
* [libyang](https://github.com/CESNET/libyang)
* libssh >= 0.9.5 (for SSH support)
* OpenSSL >= 3.0.0 or MbedTLS >= 3.5.0 (for TLS support)
* curl >= 7.30.0

#### Optional

* libpam (for PAM-based SSH `keyboard-interactive` authentication method)
* libval (only for DNSSEC SSHFP retrieval)
  * [DNSSEC-Tools/dnssec-tools/validator](https://github.com/DNSSEC-Tools/DNSSEC-Tools/tree/master/dnssec-tools/validator)
    part of the DNSSEC-Tools suite
* doxygen (for generating documentation)
* cmocka >= 1.0.1 (for tests only, see [Tests](#Tests))
* valgrind (for enhanced testing)
* gcov (for code coverage)
* lcov (for code coverage)
* genhtml (for code coverage)

## Building

```
$ mkdir build; cd build
$ cmake ..
$ make
# make install
```

The library documentation can be generated directly from the source codes using
Doxygen tool:
```
$ make doc
```

## Build Options

There are various options to change result of building.

### Changing Compiler

Set `CC` environment variable:

```
$ CC=/usr/bin/clang cmake ..
```

### Installation Prefix

By default, the library is installed with the `/usr/local` prefix, to change
it, use the following option:
```
$ cmake -DCMAKE_INSTALL_PREFIX:PATH=/usr ..
```

### Transport Protocol Support

The NETCONF protocol specification allows to use the protocol on top of
several transport protocols. **libnetconf2** provides support for SSH and
TLS transport. By default, both SSH and TLS transport is enabled. Disabling
and enabling both the transport protocols can be made
in the same way. The following command has actually the same effect as
specifying no option since it specifies the default settings.
```
$ cmake -DENABLE_SSH_TLS=ON ..
```

### DNSSEC SSHFP Retrieval

In SSH connections, if the remote NETCONF server supports it and it is
enabled, it is possible to safely retrieve server host key fingerprints
using DNSSEC and automatically consider them to be trusted without any
interaction. Enable it with the following command.
```
$ cmake -DENABLE_DNSSEC=ON ..
```

### Build Modes

There are two build modes:
* Release.
  This generates library for the production use without any debug information.
* Debug.
  This generates library with the debug information and disables optimization
  of the code.

The `Debug` mode is currently used as the default one. to switch to the
`Release` mode, enter at the command line:
```
$ cmake -D CMAKE_BUILD_TYPE:String="Release" ..
```

### Inactive Read Timeout

It is possible to adjust inactive read timeout. It is used when a new message is
being read and no new data had arrived for this amount of seconds. 20 is the default value.

```
$ cmake -D READ_INACTIVE_TIMEOUT:String="20" ..
```

### Active Read Timeout

Active read timeout is used to limit the maximum number of seconds a message is given
to arrive in its entirety once a beginning is read. The default is 300 (5 minutes).

```
$ cmake -D READ_ACTIVE_TIMEOUT:String="300" ..
```

### PSPoll Thread Count

This value limits the maximum number of threads that can concurrently access
(wait for access) a single pspoll structure. To simplify, how many threads could
simultaneously call a function whose parameter is one and the same pspoll structure.
If using **netopeer2-server**, it will warn that this value needs to be adjusted if
too small.

```
$ cmake -D MAX_PSPOLL_THREAD_COUNT:String="6" ..
```

### Code Coverage

Based on the tests run, it is possible to generate code coverage report. But
it must be enabled and these commands are needed to generate the report:
```
$ cmake -DENABLE_COVERAGE=ON ..
$ make
$ make coverage
```

Note that `gcc` compiler is required for this option.

### CMake Notes

Note that, with CMake, if you want to change the compiler or its options after
you already ran CMake, you need to clear its cache first - the most simple way
to do it is to remove all content from the 'build' directory.

## Usage

All public functions are available via 2 headers:
```
#include <nc_server.h>
#include <nc_client.h>
```

You need to include either one if implementing a NETCONF server or a NETCONF client,
respectively.

To compile your program with libnetconf2, it is necessary to link it with it using the
following linker parameters:
```
-lnetconf2
```

## Examples

See [examples](examples) directory for an example client and server.

## Tests

The repository includes several tests built with [cmocka](https://cmocka.org/).
The tests can be found in `tests` subdirectory and they are designed for
checking library functionality after code changes.

The tests are by default built in the `Debug` build mode by running
```
$ make
```

In case of the `Release` mode, the tests are not built by default (it requires
additional dependency), but it can be enabled via cmake option:
```
$ cmake -DENABLE_TESTS=ON ..
```

Note that if the necessary [cmocka](https://cmocka.org/) headers are not present
in the system include paths, tests are not available despite the build mode or
cmake's options.

Tests can be run by the make's `test` target:
```
$ make test
```

## O-RAN.WG4.TS.MP.0-R004-v17.00 defines O-RAN YANG models that import the following externally defined YANG models:

|YANG Module Name              |                Namespace                                | Revision Date
|------------------------------|---------------------------------------------------------|---------------
|iana-crypt-hash               |urn:ietf:params:xml:ns:yang:iana-crypt-hash              | 2014-08-06
|iana-hardware                 |urn:ietf:params:xml:ns:yang:iana-hardware                | 2018-03-13
|iana-if-type                  |urn:ietf:params:xml:ns:yang:iana-if-type                 | 2017-01-19
|ieee802-dot1x                 |urn:ieee:std:802.1X:yang:ieee802-dot1x                   | 2020-02-18
|ieee802-dot1x-types           |urn:ieee:std:802.1X:yang:ieee802-dot1x-types             | 2020-02-18
|ieee802-dot1x-eapol           |urn:ieee:std:802.1X:yang:ieee802-dot1x-eapol             | 2022-05-25
|ieee802-types                 |urn:ieee:std:802.1Q:yang:ieee802-types                   | 2020-06-04
|ietf-crypto-types             |urn:ietf:params:xml:ns:yang:ietf-crypto-types            | 2024-10-10
|ietf-datastores               |urn:ietf:params:xml:ns:yang:ietf-datastores              | 2018-02-14
|ietf-dhcpv6-common            |urn:ietf:params:xml:ns:yang:ietf-dhcpv6-common           | 2021-01-29
|ietf-dhcpv6-types             |urn:ietf:params:xml:ns:yang:ietf-dhcpv6-types            | 2018-09-04
|ietf-hardware                 |urn:ietf:params:xml:ns:yang:ietf-hardware                | 2018-03-13
|ietf-inet-types               |urn:ietf:params:xml:ns:yang:ietf-inet-types              | 2013-07-15
|ietf-interfaces               |urn:ietf:params:xml:ns:yang:ietf-interfaces              | 2018-02-20
|ietf-ip                       |urn:ietf:params:xml:ns:yang:ietf-ip                      | 2018-02-22
|ietf-keystore                 |urn:ietf:params:xml:ns:yang:ietf-keystore                | 2024-10-10
|ietf-netconf                  |urn:ietf:params:xml:ns:netconf:base:1.0                  | 2011-06-01
|ietf-netconf-acm              |urn:ietf:params:xml:ns:yang:ietf-netconf-acm             | 2018-02-14
|ietf-netconf-monitoring       |urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring      | 2010-10-04
|ietf-netconf-notifications    |urn:ietf:params:xml:ns:yang:ietf-netconf-notifications   | 2012-02-06
|ietf-network-instance         |urn:ietf:params:xml:ns:yang:ietf-network-instance        | 2019-01-21
|ietf-restconf                 |urn:ietf:params:xml:ns:yang:ietf-restconf                | 2017-01-26
|ietf-subscribed-notifications |urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications| 2019-09-09
|ietf-system                   |urn:ietf:params:xml:ns:yang:ietf-system                  | 2014-08-06
|ietf-truststore               |urn:ietf:params:xml:ns:yang:ietf-truststore              | 2024-10-10
|ietf-yang-library             |urn:ietf:params:xml:ns:yang:ietf-yang-library            | 2019-01-04
|ietf-yang-schema-mount        |urn:ietf:params:xml:ns:yang:ietf-yang-schema-mount       | 2019-01-14
|ietf-yang-types               |urn:ietf:params:xml:ns:yang:ietf-yang-types              | 2013-07-15
|ietf-x509-cert-to-name        |urn:ietf:params:xml:ns:yang:ietf-x509-cert-to-name       | 2014-12-10
|ieee802-dot1q-cfm             |urn:ieee:std:802.1Q:yang:ieee802-dot1q-cfm               | 2022-01-19
|ieee802-dot1q-cfm-types       |urn:ieee:std:802.1Q:yang:ieee802-dot1q-cfm-types         | 2020-06-04
|ieee802-dot1q-types           |urn:ieee:std:802.1Q:yang:ieee802-dot1q-types             | 2020-06-04
