#!/bin/bash
VERSION=2.2.0-4
mkdir -p .ubuntu/cec-daemon_$VERSION/usr/local/bin/
cp ./cec-daemon .ubuntu/cec-daemon_$VERSION/usr/local/bin/
mkdir -p .ubuntu/cec-daemon_$VERSION/DEBIAN
cat > .ubuntu/cec-daemon_$VERSION/DEBIAN/control <<EOF
Package: cec-daemon
Version: $VERSION
Section: base
Priority: optional
Architecture: i386
Depends: 
Maintainer: spilikin.com
Description: CEC Daemon
EOF
cd .ubuntu/
dpkg-deb --build cec-daemon_$VERSION

