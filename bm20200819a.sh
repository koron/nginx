#!/bin/bash

set -eu

H=127.0.0.1
AB=ab

N=1000
Cs=( 2 4 8 16 )
URLs=( "http://${H}/test0/" "http://${H}/test1/" )
Ms=( GET PO0 PO1 )
L=10

json=bm20200819a.json

#URLs=( http://${H}/test0/ )
#URLs=( "http://${H}/test0/" "http://${H}/test0/" )
#Ms=( GET PO0 PO1 )
N=1
Cs=( 1 )
L=1

get_rps() {
  awk '/^Requests per second:/{ print $4 }'
}

run_ab() {
  c=$1 ; shift
  u=$1 ; shift
  ${AB} -n $N -c $c -q "$@" $u | get_rps
}

run_bench() {
  c=$1 ; shift
  u=$1 ; shift
  m=$1 ; shift
  l=$1 ; shift
  echo -n "attempt: m=$m u=$u c=$c l=$l	"
  case $m in
    GET)
      run_ab $c $u
      ;;
    PO0)
      run_ab $c $u -T "application/json" -p ${json}
      ;;
    PO1)
      run_ab $c $u -T "application/json" -p ${json}.gz -H "Content-Encoding: gzip"
      ;;
  esac
}

intermission() {
  s=$1 ; shift
  echo "waiting ${s} seconds..." 2>&1
  sleep $s
}

if [ ${json} -nt ${json}.gz ] ; then
  gzip -kf ${json}
fi

for c in "${Cs[@]}" ; do
  for m in "${Ms[@]}" ; do
    for u in "${URLs[@]}" ; do
      for l in `seq 1 $L` ; do
        run_bench $c $u $m $l
        #intermission 3
      done
      #intermission 10
    done
  done
done
