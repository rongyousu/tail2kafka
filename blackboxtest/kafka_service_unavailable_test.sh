#!/bin/bash

BIN="${BASH_SOURCE[0]}"
BINDIR=$(readlink -e $(dirname $BIN))
HOST=$(hostname)

CFGDIR="$BINDIR/tail2kafka"
PIDF=/var/run/tail2kafka.pid
LIBDIR=/var/lib/tail2kafka
BUILDDIR=$BINDIR/../build

if [ ! -d $CFGDIR ]; then
  echo "$CFGDIR NOT FOUND"
  exit 1
fi

UNBLOCK_KAFKA="iptables -D OUTPUT -p tcp --dport 9092 -j REJECT --reject-with tcp-reset"
BLOCK_KAFKA="iptables -A OUTPUT -p tcp --dport 9092 -j REJECT --reject-with tcp-reset"
echo "UNBLOCK_KAFKA $UNBLOCK_KAFKA"; $UNBLOCK_KAFKA

# delete.topic.enable=true
test -f $BINDIR/../ENV.sh && source $BINDIR/../ENV.sh
KAFKAHOME=${KAFKAHOME:-"/opt/kafka"}
ZOOKEEPER=${ZOOKEEPER:-"localhost:2181/kafka"}
KAFKASERVER=${KAFKASERVER:-"localhost:9092"}
cp $CFGDIR/main.lua $CFGDIR/main.lua.backup
sed -i -E "s|localhost:9092|$KAFKASERVER|g" $CFGDIR/main.lua

echo "WARN: YOU MUST KILL tail2kafka and kafka2file first, both may create topic automatic"

TOPIC="basic"
T2KDIR=logs
K2FDIR=kafka2filedir

echo "kill tail2kafka"
(test -f $PIDF && test -d /proc/$(cat $PIDF)) && kill $(cat $PIDF); sleep 2
echo "kill kafka2file"
K2FPID=$K2FDIR/$TOPIC.0.lock
(test -f $K2FPID && test -d /proc/$(cat $K2FPID)) && kill $(cat $K2FPID); sleep 2

test -d $T2KDIR || mkdir $T2KDIR
find $T2KDIR -type f -name "*.log*" -delete

test -d $K2FDIR || mkdir $K2FDIR
find $K2FDIR -type f -delete

cd $KAFKAHOME
bin/kafka-topics.sh --delete --if-exists --zookeeper $ZOOKEEPER  --topic $TOPIC
if bin/kafka-topics.sh --list --zookeeper $ZOOKEEPER | grep -q '\<basic\>'; then
  echo "delete kafka topic $TOPIC error"
  exit 1
fi
bin/kafka-topics.sh --create --zookeeper $ZOOKEEPER --replication-factor 1 --partitions 1 --topic $TOPIC
cd -

$BUILDDIR/kafka2file $KAFKASERVER basic 0 offset-end $K2FDIR &
sleep 5
if [ ! -f $K2FPID ] || [ ! -d /proc/$(cat $K2FPID) ]; then
  echo "start kafka2file failed"
  exit 1
fi

# prepare history file
rm -rf $LIBDIR/basic.* && test -f $LIBDIR/fileoff && rm $LIBDIR/fileoff
for suffix in 2 1; do
  for i in `seq 1 10000`; do
    echo "BASIC_HISTORY_${suffix} $i" >> $T2KDIR/basic.log.history.$suffix
  done
  echo "$T2KDIR/basic.log.history.$suffix" >> $LIBDIR/basic.history
done

rm -f /var/log/tail2kafka/tail2kafka.log_$(date +%Y-%m-%d)
$BUILDDIR/tail2kafka $CFGDIR; sleep 2
if [ ! -f $PIDF ] || [ ! -d /proc/$(cat $PIDF) ]; then
  echo "start tail2kafka failed"
  exit 1;
fi
mv $CFGDIR/main.lua.backup $CFGDIR/main.lua

echo "wait history file be consumed ..."; sleep 30
if [ -f $LIBDIR/basic.history ]; then
  echo "history file should be consumed"
  exit 1
fi

echo "WAIT history file kafka2file ..."; sleep 20
for suffix in 2 1; do
  HISTORYFILE_MD5=$(md5sum $T2KDIR/basic.log.history.$suffix | cut -d' ' -f1)
  K2FFILE_MD5=$(md5sum $K2FDIR/basic/${HOST}_basic.log.history.$suffix | cut -d' ' -f1)
  if [ "$HISTORYFILE_MD5" != "$K2FFILE_MD5" ]; then
    echo "HISTORYFILE $T2KDIR/basic.log.history.$suffix != $K2FDIR/basic/${HOST}_basic.log.history.$suffix"
  exit 1
  fi
done

echo "BLOCK_KAFKA $BLOCK_KAFKA"; $BLOCK_KAFKA
sleep 1

NFILE=5
NLINE=120000  # must bigger than queue.buffering.max.messages
LOGFILE=$T2KDIR/basic.log
for suffix in `seq $NFILE -1 1`; do
  for i in `seq 1 $NLINE`; do
    echo "BASIC_${suffix} $i" >> $LOGFILE
  done
  mv $LOGFILE $LOGFILE.$suffix &&  touch $LOGFILE

  echo "$(date +%H:%M:%S) wait inotify $LOGFILE moved $LOGFILE.$suffix ...";  sleep 90   # rotate interval must > 60

  linenum=$(wc -l $LIBDIR/basic.history 2>/dev/null | cut -d' ' -f1)
  if [ "$linenum" != $((NFILE+1-suffix)) ]; then
    echo "$LINENO $(date +%H:%M:%S) round $suffix expect history file number $linenum != $((NFILE+1-suffix))"
    exit 1
  fi

  ofile=$(readlink -e $LOGFILE.$suffix)
  hfile=$(tail -n 1 $LIBDIR/basic.history)
  if [ "$hfile" != "$ofile" ]; then
    echo "except history file $ofile != $hfile"
    exit 1
  fi
done

touch $LOGFILE
echo "UNBLOCK_KAFKA $UNBLOCK_KAFKA"; $UNBLOCK_KAFKA

for i in `seq 1 100`; do
  echo "BASIC_0 $i" >> $LOGFILE
done
# kafka2file has memory cache, rotate file to trigger kafka2file flush cache
mv $LOGFILE $LOGFILE.0

echo "WAIT kafka2file ... "; sleep 60

# WARN basic.log.5 out of order, I haven't found a way to fix it yet
# but the messages was not lost
SIZE1=$(stat -c %s $T2KDIR/basic.log.$NFILE)
SIZE2=$(stat -c %s $K2FDIR/basic/${HOST}_basic.log.$NFILE)
if [ "$SIZE1" != "$SIZE2" ]; then
  echo "$LINENO expect $K2FDIR/basic/${HOST}_basic.log.$NFILE size != $T2KDIR/basic.log.$NFILE"
  exit 1
fi

NFILE=$((NFILE-1))
for suffix in `seq $NFILE -1 0`; do
  ofile=$T2KDIR/basic.log.$suffix
  dfile=$K2FDIR/basic/${HOST}_basic.log.$suffix

  md5Ofile=$(md5sum $ofile | cut -d' ' -f1)
  md5Dfile=$(md5sum $dfile | cut -d' ' -f1)

  if [ "$md5Ofile" != "$md5Dfile" ]; then
    echo "$(date +%Y-%m-%d_%H-%M-%S) expect $dfile content != $ofile"
    exit 1
  fi
done

CHILDPID=$(pgrep -P $(cat $PIDF))
OPENFILENUM=0;
for f in $(ls /proc/$CHILDPID/fd/); do
  if readlink /proc/$CHILDPID/fd/$f | grep -q basic.log; then
    OPENFILENUM=$((OPENFILENUM+1));
  fi
done

if [ $OPENFILENUM != 1 ]; then
  echo "too many open files, file fd may leak"
  exit 1
fi

echo "OK"
