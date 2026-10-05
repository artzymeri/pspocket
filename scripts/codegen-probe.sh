#!/bin/zsh
# Route B's go/no-go on the phone (D-147, D-150): does iOS run libraries the app did not ship with?
#
#   scripts/codegen-probe.sh [--identity]
#
# Builds three small libraries whose ps_answer() returns 42, signs them on the Mac with the Apple Development
# certificate in the installed app's profile, copies them to the app's Library/Application Support/pspocket-codegen/,
# launches the app's self-test and prints what the codegen.dlopen probe saw:
#   clang-bundleid.dylib    made by clang, signed by codesign with the app's bundle ID as identifier (LiveContainer's way)
#   clang-filename.dylib    the same with codesign's default identifier
#   ours-bundleid.dylib     made by our Mach-O writer (runtime/codesign), signed by codesign
# With --identity it first runs `scripts/ios.sh push-signing-identity`, so the app also writes and signs a library
# itself (the case route B needs). The phone must be connected and unlocked, with the app installed by
# scripts/ios.sh install. Everything stays in inventory/codegen-probe and on the phone (never committed).
set -eu
cd "$(dirname "$0")/.."
bundle=com.artzymeri.pspocket.testhost
out=inventory/codegen-probe
build=${PSPOCKET_HOST_BUILD:-build/host}
mkdir -p $out
[[ ${1:-} == --identity ]] && scripts/ios.sh push-signing-identity

identity=$(security find-identity -v -p codesigning | awk '/Apple Development/ {print $2; exit}')
[[ -n $identity ]] || { echo "no Apple Development identity in the keychain" >&2; exit 1; }
app=$(cat build/ios/last-build-iphoneos 2>/dev/null || true)
if [[ -n $app && -f $app/embedded.mobileprovision ]]; then
  # The certificate must be the one the installed profile lists (LiveContainer's condition).
  security cms -D -i $app/embedded.mobileprovision > $out/profile.plist
  /usr/bin/python3 - $out/profile.plist $identity <<'EOF' || { echo "the keychain identity is not in the app's profile" >&2; exit 1; }
import plistlib, sys, hashlib
p = plistlib.load(open(sys.argv[1], 'rb'))
sys.exit(0 if any(hashlib.sha1(c).hexdigest().upper() == sys.argv[2] for c in p.get('DeveloperCertificates', [])) else 1)
EOF
fi

print 'int ps_answer(void) { return 42; }' > $out/answer.c
xcrun -sdk iphoneos clang -target arm64-apple-ios17.0 -dynamiclib -install_name @rpath/answer.dylib -O2 \
  -o $out/clang.dylib $out/answer.c
cp $out/clang.dylib $out/clang-bundleid.dylib
cp $out/clang.dylib $out/clang-filename.dylib
codesign -f -s $identity -i $bundle --timestamp=none $out/clang-bundleid.dylib 2>/dev/null
codesign -f -s $identity --timestamp=none $out/clang-filename.dylib 2>/dev/null
$build/runtime/codesign/pspocket-codesign answer $out/ours.dylib --platform ios --min-os 17.0 > /dev/null
cp $out/ours.dylib $out/ours-bundleid.dylib
codesign -f -s $identity -i $bundle --timestamp=none $out/ours-bundleid.dylib 2>/dev/null
for f in clang-bundleid clang-filename ours-bundleid; do
  codesign --verify $out/$f.dylib || { echo "codesign rejects $f.dylib on the Mac" >&2; exit 1; }
done

dev=$(xcrun devicectl list devices 2>/dev/null | awk '/iPhone/ && /connected|available \(paired\)/ {print $3; exit}')
[[ -n $dev ]] || { echo "the iPhone is not connected: plug it in and unlock it" >&2; exit 2; }
for f in clang-bundleid clang-filename ours-bundleid; do
  xcrun devicectl device copy to --device $dev --domain-type appDataContainer --domain-identifier $bundle \
    --source $out/$f.dylib --destination "Library/Application Support/pspocket-codegen/$f.dylib" > /dev/null
done
echo "==> libraries on the phone; running the self-test"
scripts/ios.sh run --autorun --exit > $out/run.log 2>&1 || true
scripts/ios.sh report > $out/report.txt 2>&1 || true
grep -A 12 "codegen.dlopen" $out/report.txt || { echo "no codegen.dlopen in the report ($out/report.txt)"; exit 1; }
