#!/bin/bash
# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
#
# Byte-parity harness: /usr/libexec/PlistBuddy (oracle) vs our PlistBuddy.
# Each case runs both tools in their own copy of the same fixture directory
# and compares stdout, stderr, exit status and every file left behind.
#
#   ./tests/parity.sh          # MY defaults to build/release/PlistBuddy
#   MY=build/debug/PlistBuddy ./tests/parity.sh
#
# A relative MY is resolved against the project root so the Makefile and the
# Xcode build phase can pass a short path.

ORACLE=${ORACLE:-/usr/libexec/PlistBuddy}
HERE=$(cd "$(dirname "$0")" && pwd)
PROJROOT=$(cd "$HERE/.." && pwd)
MY=${MY:-build/release/PlistBuddy}
case $MY in /*) ;; *) MY=$PROJROOT/$MY ;; esac
[ -x "$MY" ] || { echo "parity: $MY not built" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/plistbuddy-parity.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
FIX=$WORK/fixtures
mkdir -p "$FIX"
export TZ=America/Los_Angeles

# Both tools print their own argv[0] basename in usage, so ours gets the
# oracle's name.  The oracle runs in place: AMFI kills a copied platform
# binary.
mkdir -p "$WORK/bin"
cp "$MY" "$WORK/bin/PlistBuddy"

cat > "$FIX/f.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key>
	<string>org.libredarwin.test</string>
	<key>Count</key>
	<integer>42</integer>
	<key>Neg</key>
	<integer>-9000000000</integer>
	<key>Ratio</key>
	<real>0.1</real>
	<key>Enabled</key>
	<true/>
	<key>Off</key>
	<false/>
	<key>When</key>
	<date>2024-02-29T12:34:56Z</date>
	<key>Blob</key>
	<data>aGVsbG8gd29ybGQ=</data>
	<key>Bin</key>
	<data>AAEC/w==</data>
	<key>Types</key>
	<array>
		<dict>
			<key>Name</key>
			<string>Doc</string>
			<key>Exts</key>
			<array>
				<string>txt</string>
				<string>md</string>
			</array>
		</dict>
		<string>second</string>
		<array/>
		<dict/>
	</array>
	<key>Nested</key>
	<dict>
		<key>a</key>
		<dict>
			<key>b</key>
			<string>deep</string>
		</dict>
		<key>with:colon</key>
		<string>escaped</string>
		<key>sp ace</key>
		<string>spaced</string>
	</dict>
	<key>Unicode</key>
	<string>héllo wörld ✓</string>
</dict>
</plist>
EOF
cp "$FIX/f.plist" "$FIX/bin.plist"
plutil -convert binary1 "$FIX/bin.plist"
cat > "$FIX/arr.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
	<string>zero</string>
	<integer>1</integer>
	<dict><key>k</key><string>v</string></dict>
</array>
</plist>
EOF
cat > "$FIX/m.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Count</key>
	<integer>7</integer>
	<key>Fresh</key>
	<string>new</string>
	<key>Nested</key>
	<dict>
		<key>a</key>
		<dict>
			<key>c</key>
			<string>merged</string>
		</dict>
		<key>z</key>
		<integer>26</integer>
	</dict>
	<key>Types</key>
	<array>
		<string>appended</string>
	</array>
</dict>
</plist>
EOF
cat > "$FIX/ma.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
	<string>m1</string>
	<string>m2</string>
</array>
</plist>
EOF
echo '<plist><dict><key>x</key>' > "$FIX/bad.plist"
printf 'raw\x00bytes\xff\n' > "$FIX/blob.bin"
printf 'text from file' > "$FIX/str.txt"
printf '12' > "$FIX/num.txt"
ln -s f.plist "$FIX/link.plist"

pass=0
fail=0

# t STDIN ARG...: run one case; STDIN is fed to both tools ("" for none).
t() {
	local input=$1; shift
	local a=$WORK/a b=$WORK/b
	rm -rf "$a" "$b"
	cp -R "$FIX" "$a"
	cp -R "$FIX" "$b"
	# Some inputs crash both tools; the outer 2>/dev/null drops bash's
	# "Segmentation fault" notice, the status still records it.
	(cd "$a" && printf '%s' "$input" | "$ORACLE" "$@" \
	    >"$WORK/a.out" 2>"$WORK/a.err"; echo $? >"$WORK/a.rc") 2>/dev/null
	(cd "$b" && printf '%s' "$input" | "$WORK/bin/PlistBuddy" "$@" \
	    >"$WORK/b.out" 2>"$WORK/b.err"; echo $? >"$WORK/b.rc") 2>/dev/null
	local why=
	cmp -s "$WORK/a.out" "$WORK/b.out" || why="$why stdout"
	cmp -s "$WORK/a.err" "$WORK/b.err" || why="$why stderr"
	cmp -s "$WORK/a.rc" "$WORK/b.rc" || why="$why status"
	diff -r "$a" "$b" >/dev/null 2>&1 || why="$why files"
	if [ -z "$why" ]; then
		pass=$((pass + 1))
		return
	fi
	fail=$((fail + 1))
	echo "FAIL:$why: $*"
	case $why in *stdout*) diff "$WORK/a.out" "$WORK/b.out" | head -10 ;; esac
	case $why in *stderr*) diff "$WORK/a.err" "$WORK/b.err" | head -10 ;; esac
	case $why in *status*) echo "  oracle $(cat "$WORK/a.rc") ours $(cat "$WORK/b.rc")" ;; esac
	case $why in *files*) diff -r "$a" "$b" | head -10 ;; esac
}

# --- arguments and usage
t ""
t "" -c
t "" -x
t "" -l
t "" -h
t "" --help
t "" -help
t "" f.plist arr.plist
t "" -c Print
t "" missing.plist -c Print
t "" -c Frob f.plist
t "" -c Print -c Frob f.plist
t "" bad.plist -c Print
t "" -l link.plist -c Print
t "" link.plist -c Print
t "" -c 'Print :Count' -x f.plist
t "" -q
# /tmp/, /etc/ and /var/ prefixes are rewritten to /private/... (any case);
# Print leaves nothing dirty, so nothing is written there.
t "" -c Print /tmp/plistbuddy-parity-absent.plist
t "" -c Print /TMP/plistbuddy-parity-absent.plist
t "" -c Print /var/plistbuddy-parity-absent.plist
t "" -c Print "/tmp/$(printf '%01100d' 0)"
t "" -c Print "$(printf '%01100d' 0)"

# --- Print
t "" -c Print f.plist
t "" -x -c Print f.plist
t "" -c Print bin.plist
t "" -c Print arr.plist
t "" -c 'Print :' f.plist
for e in :Count :Neg :Ratio :Enabled :Off :When :Blob :Bin :Types :Types:0 \
    :Types:0:Exts:1 :Types: :Types:9 :Types:-1 :Nested:a:b ':Nested:with\:colon' \
    ':Nested:sp ace' "':Nested:sp ace'" '":Nested:sp ace"' :Unicode :Missing \
    :Count:x Nested '"unclosed' "'unclosed"; do
	t "" -c "Print $e" f.plist
	t "" -x -c "Print $e" f.plist
done
t "" -c 'ls :Count' f.plist
t "" -c 'print :count' f.plist
t "" -c 'PRINT :Count' f.plist
t "" -c 'Printer' f.plist
t "" -c 'Print :1' arr.plist
t "" -c 'Print 2' arr.plist

# --- Set
t "" -c 'Set :Count 7' f.plist
t "" -c 'Set :Count 0x10' f.plist
t "" -c 'Set :Count abc' f.plist
t "" -c 'Set :Ratio 2.5' f.plist
t "" -c 'Set :Ratio x' f.plist
t "" -c 'Set :Enabled no' f.plist
t "" -c 'Set :Off YES' f.plist
t "" -c 'Set :Blob new data' f.plist
t "" -c 'Set :CFBundleIdentifier com.example.x' f.plist
t "" -c 'Set :CFBundleIdentifier "two  words"' f.plist
t "" -c 'Set :CFBundleIdentifier a\tb\nc' f.plist
t "" -c "Set :When 'Mon Jan 01 00:00:00 PST 2024'" f.plist
t "" -c 'Set :When 01/02/24' f.plist
t "" -c 'Set :When garbage' f.plist
t "" -c 'Set :Types x' f.plist
t "" -c 'Set :Missing x' f.plist
t "" -c 'Set :Types:1 changed' f.plist
t "" -c 'Set :Types:0:Name Renamed' f.plist
t "" -c 'Set' f.plist
t "" -c 'Set :Count' f.plist
t "" -c 'Set :Count "open' f.plist
t "" -c '= :Count 3' f.plist
t "" -c 'Set :1 9' arr.plist

# --- Add
for ty in string array dict bool real integer date data; do
	t "" -c "Add :New $ty" f.plist
	t "" -c "Add :New $ty 1" f.plist
done
t "" -c 'Add :New string hello world' f.plist
t "" -c 'Add :New string "quoted value" tail' f.plist
t "" -c "Add :New date 'Thu Feb 29 04:34:56 PST 2024'" f.plist
t "" -c 'Add :New bogus x' f.plist
t "" -c 'Add :New' f.plist
t "" -c 'Add' f.plist
t "" -c 'Add :Count integer 1' f.plist
t "" -c 'Add :Count:x string 1' f.plist
t "" -c 'Add :A:B:C string deep' f.plist
t "" -c 'Add :Types: string end' f.plist
t "" -c 'Add :Types:0 string front' f.plist
t "" -c 'Add :Types:2 string mid' f.plist
t "" -c 'Add :Types:99 string far' f.plist
t "" -c 'Add :Types:0:Exts: string rtf' f.plist
t "" -c 'Add :Types:7:x string far' f.plist
t "" -c 'Add :k\:x string esc' -c 'Print' f.plist
t "" -c 'Add ":sp ace" string s' -c Print f.plist
t "" -c 'Add :New string "open' f.plist
t "" -c '+ :New integer 5' f.plist
t "" -c 'Add : string x' f.plist
t "" -c 'Add :3 string x' arr.plist
t "" -c 'Add : string x' arr.plist
t "" -c 'Add :New string x' new.plist
t "" -c 'Add :a integer 1' -c 'Add :b integer 2' -c 'Add :c integer 3' \
    -c 'Add :d integer 4' -c 'Add :e integer 5' -c Print new.plist

# --- Copy
t "" -c 'Copy :Count :Count2' f.plist
t "" -c 'Copy :Nested :Nested2' -c 'Set :Nested2:a:b shared' -c Print f.plist
t "" -c 'Copy :Types :T2' f.plist
t "" -c 'Copy :Enabled :E2' -c 'Copy :When :W2' -c 'Copy :Blob :B2' -c 'Copy :Ratio :R2' f.plist
t "" -c 'Copy :Missing :x' f.plist
t "" -c 'Copy :Count :Nested' f.plist
t "" -c 'Copy :Count :Count:x' f.plist
t "" -c 'Copy :Count' f.plist
t "" -c 'Copy' f.plist
t "" -c 'cp :Count :Types:0' f.plist
t "" -c 'Copy :Count :X:Y' f.plist

# --- Delete
t "" -c 'Delete :Count' f.plist
t "" -c 'Delete :Types:0' f.plist
t "" -c 'Delete :Types:9' f.plist
t "" -c 'Delete :Types:' f.plist
t "" -c 'Delete :Missing' f.plist
t "" -c 'Delete' f.plist
t "" -c 'Delete :' f.plist
t "" -c 'Remove :Count' -c 'rm :Neg' -c '- :Ratio' f.plist
t "" -c 'Delete :Count:x' f.plist
t "" -c 'CD :Nested' -c 'Delete :Nested' -c Print f.plist

# --- CD
t "" -c 'CD :Nested' -c Print -c 'Print a' -c 'Add q string w' -c 'Print' f.plist
t "" -c 'CD :Count' f.plist
t "" -c 'CD :Missing' f.plist
t "" -c 'CD' -c Print f.plist
t "" -c 'CD :Types' -c 'Print 0' -c 'Add : string x' -c 'Print' f.plist
t "" -c 'CD :Nested:a' -c 'Print b' -c 'CD :' -c 'Print :Count' f.plist

# --- Merge
t "" -c 'Merge m.plist' f.plist
t "" -c 'Merge -r m.plist' f.plist
t "" -c 'Merge -R m.plist' f.plist
t "" -c 'Merge m.plist :Nested' f.plist
t "" -c 'Merge -r m.plist :Nested' f.plist
t "" -c 'Merge ma.plist :Types' f.plist
t "" -c 'Merge ma.plist' f.plist
t "" -c 'Merge m.plist :Types' f.plist
t "" -c 'Merge ma.plist :Nested' f.plist
t "" -c 'Merge m.plist :Count' f.plist
t "" -c 'Merge m.plist :Missing' f.plist
t "" -c 'Merge missing.plist' f.plist
t "" -c 'Merge bad.plist' f.plist
t "" -c 'Merge str.txt' f.plist
t "" -c 'Merge' f.plist
t "" -c 'Merge -r' f.plist
t "" -c 'Merge ma.plist' arr.plist
t "" -c 'Merge m.plist' arr.plist
t "" -c 'Merge f.plist' -c Print f.plist

# --- Import
t "" -c 'Import :Raw blob.bin' -c 'Print :Raw' f.plist
t "" -c 'Import :Blob blob.bin' f.plist
t "" -c 'Import :CFBundleIdentifier str.txt' f.plist
t "" -c 'Import :Count num.txt' f.plist
t "" -c 'Import :Types blob.bin' f.plist
t "" -c 'Import :Raw missing' f.plist
t "" -c 'Import :Raw' f.plist
t "" -c 'Import' f.plist
t "" -c 'Import :A:B blob.bin' f.plist

# --- Clear, Save, Revert, Help, Exit
for ty in "" string array dict bool real integer data bogus; do
	t "" -c "Clear $ty" -c Print f.plist
done
t "" -c 'Clear date' f.plist
t "" -c 'Add :x string y' -c Save -c 'Add :z string w' f.plist
t "" -c 'Add :x string y' -c Revert -c Print f.plist
t "" -c 'Add :x string y' -c Revert new.plist
t "" -c Help f.plist
t "" -c '?' f.plist
t "" -c Exit -c Print f.plist
t "" -c 'Quit' f.plist
t "" -c 'Bye' f.plist
t "" -c 'Save' bin.plist
t "" -c 'Set :Count 1' bin.plist
t "" -c 'Save' arr.plist

# --- interactive mode
t "" f.plist
t "Print :Count
" f.plist
t "Print :Count" f.plist
t "Set :Count 5
Save
Print :Count
Exit
Print
" f.plist
t "Add :x string y
Exit
" f.plist
t "

Frob
Print :Nested
help
" f.plist
t "$(printf 'Add :long string %0600d\nPrint :long\n' 0)" f.plist

echo "parity: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
