# Chinese Calendar and Weather Header Design

## Goal

Improve the 300 x 224 calendar and weather panels on the 600 x 448 e-paper
dashboard. The calendar must use larger date numbers, center the current date
inside its black highlight, start each week on Monday, and use Chinese labels.
The calendar and weather headers must also show the requested date information
in Chinese.

## Scope

- Keep the existing four-panel dashboard geometry and refresh behavior.
- Change only calendar/weather presentation and pure layout helpers needed to
  test it.
- Preserve the hourly and seven-day weather charts.
- Do not alter network fetching, time synchronization, partial refresh, PVE, or
  NAS behavior.

## Typography and Text

Use `u8g2_font_wqy16_t_gb2312` for Chinese headers, weather labels, and weekday
labels. The smaller `chinese1` font partition omits several required glyphs;
the bundled complete GB2312 font covers every requested string. It adds about
318 KB of linked Flash and leaves the compiled firmware at 72% Flash usage,
without increasing RAM or IRAM. Use UTF-8 drawing APIs so the source strings
remain readable.

Use `u8g2_font_helvB14_tf` for calendar date numbers. The current 8-point font
leaves too much unused space in cells that are approximately 41 pixels wide
and 28 pixels high. The 14-point bold face remains within those bounds while
substantially improving readability.

Display these dynamic strings:

- Calendar header: `2026年8月30日 星期日`
- Weekday row: `周一`, `周二`, `周三`, `周四`, `周五`, `周六`, `周日`
- Weather header: `今天天气 8月30日`
- Weather summary: `23.4°C 湿度67% 风速3.2km/h`
- Invalid-time fallback: `时间不可用`

The examples illustrate format only; values come from `timeinfo` and the
existing weather model.

## Calendar Layout

The calendar header occupies the top 29 pixels and retains a separator line.
The weekday row follows below it. The remaining height is divided evenly into
six date rows so every possible month has stable geometry.

Convert the C library weekday value (`Sunday == 0`) to a Monday-first column
with `(tm_wday + 6) % 7`. This places Monday in column zero and Sunday in
column six without changing date calculations.

Treat every date cell as an explicit rectangle. For each number:

1. Format the day into a short string.
2. Measure its rendered width with the selected U8g2 font.
3. Calculate the baseline from the font ascent and descent.
4. Center the measured glyph box horizontally and vertically in the cell.

For the current day, draw an inset black rectangle using the same cell
coordinates, then render the centered number in white. This removes the
existing mismatch between the highlight rectangle and the fixed cursor offset.

## Weather Layout

Use the same 29-pixel header band as the calendar. Place the Chinese current
weather summary directly below it. Move the hourly chart down enough to clear
the summary while keeping the divider and seven-day chart within the existing
224-pixel panel.

The weather header includes today's month and day. The summary uses Chinese
labels for humidity and wind while retaining compact metric values and units.
The hourly and daily plot content remains unchanged.

## Testable Boundaries

Add pure helpers to `dashboard_model.h` for behavior that can be verified on a
host compiler:

- Monday-first weekday conversion.
- Month length including Gregorian leap-year rules.
- Calendar cell placement for a given first weekday and day number.
- Centered text origin/baseline calculation from cell and font metrics.
- Chinese calendar header and weather header formatting.

Add focused assertions before production changes, run them once to observe the
expected failures, then implement the minimum code required to pass. Extend the
source verifier to ensure the production renderer uses the Chinese font,
Monday-first labels, dynamic Chinese headers, and calculated centering rather
than fixed offsets.

## Verification

Completion requires all of the following:

1. Host dashboard model tests pass, including Monday-first placement, leap
   years, Chinese header formats, and horizontal/vertical centering.
2. The source verifier passes and confirms the renderer is wired to the tested
   helpers and requested Chinese strings.
3. The complete firmware compiles with the repository build script.
4. `git diff --check` reports no whitespace errors.
5. A layout audit confirms the largest header strings and two-digit dates fit
   within their fixed panel/cell bounds without overlapping the weather charts.

Real e-paper appearance and glyph quality remain a hardware acceptance step;
the automated checks establish the geometry, text content, and build validity.
