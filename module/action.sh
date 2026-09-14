#!/system/bin/sh
MODDIR=${0%/*}
LOGTAG="CODM-TutorialSkip"

echo "=============================="
echo " CODM Tutorial Skip"
echo "=============================="
echo
echo "Status:"
if [ -f "$MODDIR/disable" ]; then
    echo "DISABLED"
else
    echo "ENABLED"
fi
echo
echo "Recent logs:"
logcat -d -s "$LOGTAG" 2>/dev/null | tail -n 30
