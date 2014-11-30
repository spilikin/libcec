make
mkdir -p .ubuntu/cec-daemon_2.2.0-2/usr/local/bin/
cp ./cec-daemon .ubuntu/cec-daemon_2.2.0-2/usr/local/bin/
mkdir -p .ubuntu/cec-daemon_2.2.0-2/DEBIAN
cat > .ubuntu/cec-daemon_2.2.0-2/DEBIAN/control <<EOF
Package: cec-daemon
Version: 2.2.0-2
Section: base
Priority: optional
Architecture: i386
Depends: 
Maintainer: spilikin.com
Description: CEC Daemon
EOF
cd .ubuntu/
dpkg-deb --build cec-daemon_2.2.0-2

