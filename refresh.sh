#!/bin/sh

set -eu

make
sudo make install

if [ -e /opt/nginx/logs/nginx.pid ] ; then
  echo "nginx: stop..."
  sudo /opt/nginx/sbin/nginx -s stop
fi
echo "nginx: clear log..."
sudo rm -f /opt/nginx/logs/*.log
echo "nginx: start..."
sudo /opt/nginx/sbin/nginx
