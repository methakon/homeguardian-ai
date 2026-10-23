#!/usr/bin/env bash
# verify_jni_exports.sh - fail when a Java `native` declaration has no matching JNI entry point.
#
# WHY THIS EXISTS, AS A TEST RATHER THAN A PARAGRAPH:
#   A Java `native` method binds to the C symbol `Java_<pkg>_<Class>_<method>`. A definition that
#   sits OUTSIDE an `extern "C"` region gets a C++-MANGLED name
#   (_Z52Java_..._nativeDrainObservationsP7_JNIEnvP8_jobject) which JNI cannot resolve, and the
#   device throws java.lang.UnsatisfiedLinkError on the FIRST call - which is exactly what shipped
#   once, from two functions appended after the file's `}  // extern "C"`.
#
#   The earlier "verification" of that wiring grepped the DEX for the Java DECLARATION and called
#   the feature verified. That was the wrong artifact: the declaration is present in the DEX whether
#   or not the native side can ever satisfy it. This check reads the compiled ELF's dynamic symbol
#   table, so the build fails HERE instead of on the operator's phone.
#
# Usage: verify_jni_exports.sh <libhg_harness.so> <java-source-dir> [llvm-nm]
set -u

SO="${1:-}"
JAVA_DIR="${2:-}"
NM="${3:-llvm-nm}"

if [ -z "$SO" ] || [ -z "$JAVA_DIR" ]; then
  echo "usage: $0 <lib.so> <java-source-dir> [llvm-nm]" >&2
  exit 2
fi
[ -f "$SO" ] || { echo "FATAL: no such shared object: $SO" >&2; exit 2; }
[ -d "$JAVA_DIR" ] || { echo "FATAL: no such java source dir: $JAVA_DIR" >&2; exit 2; }
command -v "$NM" >/dev/null 2>&1 || { echo "FATAL: $NM not found" >&2; exit 2; }

# Derive the package from the sources rather than hardcoding one. The script is now used by two
# apps with different packages (org.homeguardian.harness and org.homeguardian.edgeservice); a
# hardcoded prefix silently checked the wrong symbol names for the second one.
PKG="$(sed -nE 's/^[[:space:]]*package[[:space:]]+([A-Za-z0-9_.]+)[[:space:]]*;.*/\1/p' "$JAVA_DIR"/*.java 2>/dev/null | head -1)"
if [ -z "$PKG" ]; then
  echo "FATAL: could not read a \`package\` declaration from $JAVA_DIR/*.java" >&2
  exit 2
fi
PKG_MANGLED="$(printf '%s' "$PKG" | tr '.' '_')"
echo "    java package:             $PKG"

EXPORTS="$("$NM" -D --defined-only "$SO" | awk '{print $NF}' | grep '^Java_' | sort || true)"
MANGLED="$("$NM" -D --defined-only "$SO" | awk '{print $NF}' | grep -E '^_Z[0-9]+Java_' || true)"
DECLARED=0
MISSING=""

for JAVA_FILE in "$JAVA_DIR"/*.java; do
  [ -f "$JAVA_FILE" ] || continue
  CLASS_NAME="$(basename "$JAVA_FILE" .java)"
  # Anchored on a declaration: the line starts with an access modifier and reaches `native`
  # without crossing ';' or '=', so a string literal containing the word native (there is one:
  # "native stop requested (...)") is never mistaken for a method. `[(]` needs no escaping.
  for METHOD in $(sed -nE 's/^[[:space:]]*(public|private|protected|static)[^;=]*[[:space:]]native[[:space:]]+[A-Za-z0-9_]+[[:space:]]+([A-Za-z0-9_]+)[[:space:]]*[(].*/\2/p' "$JAVA_FILE"); do
    DECLARED=$((DECLARED + 1))
    SYMBOL="Java_${PKG_MANGLED}_${CLASS_NAME}_${METHOD}"
    if ! printf '%s\n' "$EXPORTS" | grep -qx "$SYMBOL"; then
      MISSING="$MISSING $SYMBOL"
    fi
  done
done

echo "    shared object:            $SO"
echo "    Java native declarations: $DECLARED"
echo "    unmangled Java_* exports: $(printf '%s\n' "$EXPORTS" | grep -c . || true)"

if [ -n "$MANGLED" ]; then
  echo "    WARNING: mangled Java_* symbols found (these can never bind):"
  printf '      %s\n' $MANGLED
fi

if [ "$DECLARED" -eq 0 ]; then
  # A pattern that matches nothing would otherwise report success while checking nothing - which
  # is precisely how a broken regex once passed this gate. Zero declarations is a failure.
  echo "FATAL: no Java native declarations were found under $JAVA_DIR - the check did not run,"
  echo "       so it proves nothing. Fix the pattern; do not treat this as a pass."
  exit 1
fi

if [ -n "$MISSING" ]; then
  echo "FATAL: these Java native methods have no matching C-linkage export:"
  for ONE in $MISSING; do echo "    $ONE"; done
  echo "    A C++-mangled symbol is invisible to JNI - check the extern \"C\" region(s) in the"
  echo "    matching apk*/jni/*_jni.cpp."
  exit 1
fi

echo "    OK: every Java native declaration has an exact, unmangled JNI export"
exit 0
