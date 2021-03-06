# zerod

Software broadband access server (BBRAS)

### Features

- User sessions AAA (Authentication, Authorization and Accounting)
- Rich set of various rules for user service customization
  - Firewall (restrict or grant access to specific ports)
  - Forwarding rules (as DNAT)
  - Deferred rules
  - Bandwidth limiting rules
- Realtime traffic monitoring
- Dynamic ARP inspection
- IP source verify
- HTTP URL blacklisting
- Virtual instances

### Build instructions

You need the following libraries installed:
- [cmake](http://www.cmake.org/), >= 2.8, cross-platform, open-source build system
- [libevent](http://libevent.org/), >= 2.0, event-based network I/O library
- [libconfig](https://github.com/hyperrealm/libconfig), >= 1.4, configuration management library
- [netmap](https://github.com/luigirizzo/netmap), >= 20131019, fast network packet I/O framework
- [libfreeradius-client](https://github.com/FreeRADIUS/freeradius-client), >=1.7, framework and library for writing RADIUS Clients
- [libbson](https://github.com/mongodb/libbson), >= 1.0, building, parsing, and iterating BSON documents
- [python](https://www.python.org/), >= 3.0, interactive high-level object-oriented language
- [pymongo](https://github.com/mongodb/mongo-python-driver), BSON implementation for python
- [libpcap](https://github.com/the-tcpdump-group/libpcap), the LIBpcap interface to various kernel packet capture mechanism
- [libpcre](http://www.pcre.org/), Perl Compatible Regular Expressions
- [jemalloc](https://github.com/jemalloc/jemalloc), >= 3.0, general purpose malloc(3) implementation

Build using cmake:
```bash
mkdir build
cd build
cmake ..
make
```
