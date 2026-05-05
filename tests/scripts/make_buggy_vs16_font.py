"""Build a minimal TTF that reproduces the NotoColorEmoji VS16 .null-phantom
   substitution. Outputs a few KB font with:
     gid 0: .notdef (empty outline)
     gid 1: space   (empty outline)         ← the .null-equivalent
     gid 2: warn    (a square outline)
   cmap: U+0020 → space, U+26A0 → warn
   GSUB ccmp lookup: warn + VS16 (U+FE0F) → warn + space  (ligature-style sub
       that consumes VS16 but leaves the "trailing" space glyph behind, mimicking
       NotoColorEmoji's behavior)
"""
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables import otTables as ot

fb = FontBuilder(1024, isTTF=True)

# Glyph names — gid order matters: .notdef=0, space=1, warn=2
glyph_names = [".notdef", "space", "warn", "vs16"]
fb.setupGlyphOrder(glyph_names)

# cmap
fb.setupCharacterMap({0x20: "space", 0x26A0: "warn", 0xFE0F: "vs16"})

# Glyph outlines — empty for space/.notdef, simple square for warn.
# fontTools TTGlyphPen fix the table.
def square_glyph():
    pen = TTGlyphPen(None)
    pen.moveTo((100, 100))
    pen.lineTo((900, 100))
    pen.lineTo((900, 900))
    pen.lineTo((100, 900))
    pen.closePath()
    return pen.glyph()

def empty_glyph():
    pen = TTGlyphPen(None)
    return pen.glyph()

glyphs = {
    ".notdef": empty_glyph(),
    "space": empty_glyph(),
    "warn": square_glyph(),
    "vs16": empty_glyph(),
}
fb.setupGlyf(glyphs)

# Required tables. Advance widths.
metrics = {".notdef": (1024, 0), "space": (1024, 0), "warn": (1024, 0), "vs16": (0, 0)}
fb.setupHorizontalMetrics(metrics)
fb.setupHorizontalHeader(ascent=1024, descent=0)
fb.setupOS2(sTypoAscender=1024, sTypoDescender=0, usWinAscent=1024, usWinDescent=0)
fb.setupNameTable({"familyName": "BuggyVS16Test", "styleName": "Regular"})
fb.setupPost()

# Now hand-build a GSUB ccmp lookup: when "warn" is followed by "vs16", emit
# "warn" "space". This is exactly the shape of a ligature substitution with
# expansion, but TrueType GSUB doesn't have direct N→M substitution. Instead
# we use a contextual lookup: ChainContextSubst that matches "warn vs16" and
# applies a Single Substitution mapping vs16 → space.

font = fb.font

# Lookup 0: SingleSubst — vs16 → space
single_sub = ot.SingleSubst()
single_sub.mapping = {"vs16": "space"}
single_sub_subtable = single_sub
lookup_single = ot.Lookup()
lookup_single.LookupType = 1  # single substitution
lookup_single.LookupFlag = 0
lookup_single.SubTable = [single_sub_subtable]
lookup_single.SubTableCount = 1

# Lookup 1: ChainContextSubst — match "warn vs16", apply Lookup 0 to vs16 (idx 1).
chain_sub = ot.ChainContextSubst()
chain_sub.Format = 3
chain_sub.BacktrackCoverage = []
chain_sub.BacktrackGlyphCount = 0
# Input sequence: [warn, vs16]
cov_warn = ot.Coverage(); cov_warn.glyphs = ["warn"]
cov_vs16 = ot.Coverage(); cov_vs16.glyphs = ["vs16"]
chain_sub.InputCoverage = [cov_warn, cov_vs16]
chain_sub.InputGlyphCount = 2
chain_sub.LookAheadCoverage = []
chain_sub.LookAheadGlyphCount = 0
# Apply lookup 0 (SingleSubst vs16→space) at input index 1
sub_lookup = ot.SubstLookupRecord()
sub_lookup.SequenceIndex = 1
sub_lookup.LookupListIndex = 0
chain_sub.SubstLookupRecord = [sub_lookup]
chain_sub.SubstCount = 1

lookup_chain = ot.Lookup()
lookup_chain.LookupType = 6  # chained context
lookup_chain.LookupFlag = 0
lookup_chain.SubTable = [chain_sub]
lookup_chain.SubTableCount = 1

# Build LookupList
lookup_list = ot.LookupList()
lookup_list.Lookup = [lookup_single, lookup_chain]
lookup_list.LookupCount = 2

# Feature: ccmp with lookup 1 (the chain)
feature = ot.Feature()
feature.LookupCount = 1
feature.LookupListIndex = [1]
feature.FeatureParams = None
feature_record = ot.FeatureRecord()
feature_record.FeatureTag = "ccmp"
feature_record.Feature = feature
feature_list = ot.FeatureList()
feature_list.FeatureRecord = [feature_record]
feature_list.FeatureCount = 1

# Script DFLT, language DFLT
lang_sys = ot.LangSys()
lang_sys.LookupOrder = None
lang_sys.ReqFeatureIndex = 0xFFFF
lang_sys.FeatureCount = 1
lang_sys.FeatureIndex = [0]
script = ot.Script()
script.DefaultLangSys = lang_sys
script.LangSysCount = 0
script.LangSysRecord = []
script_record = ot.ScriptRecord()
script_record.ScriptTag = "DFLT"
script_record.Script = script
script_list = ot.ScriptList()
script_list.ScriptRecord = [script_record]
script_list.ScriptCount = 1

# Assemble GSUB
gsub_table = ot.GSUB()
gsub_table.Version = 0x00010000
gsub_table.ScriptList = script_list
gsub_table.FeatureList = feature_list
gsub_table.LookupList = lookup_list

from fontTools.ttLib import newTable
font["GSUB"] = newTable("GSUB")
font["GSUB"].table = gsub_table

font.save("/tmp/buggy_vs16.ttf")
print("wrote /tmp/buggy_vs16.ttf")

# Verify
import os
print(f"size: {os.path.getsize('/tmp/buggy_vs16.ttf')} bytes")
verify = TTFont("/tmp/buggy_vs16.ttf")
print(f"  numGlyphs: {verify['maxp'].numGlyphs}")
print(f"  gid 1: {verify.getGlyphName(1)}")
print(f"  cmap[U+26A0]: {verify.getBestCmap().get(0x26A0)}")
print(f"  cmap[U+FE0F]: {verify.getBestCmap().get(0xFE0F)}")
print(f"  GSUB lookups: {len(verify['GSUB'].table.LookupList.Lookup)}")
