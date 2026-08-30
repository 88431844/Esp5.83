# Chinese Calendar and Weather Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render a larger, correctly centered Monday-first Chinese calendar and Chinese date-aware weather header in the existing 300 x 224 e-paper panels.

**Architecture:** Put date arithmetic, header formatting, cell placement, and text-centering math in dependency-free helpers in `dashboard_model.h`, with host assertions in the existing dashboard model test. Keep hardware rendering in the main Arduino sketch and guard its wiring with the existing source verifier. Reuse U8g2's bundled 16-pixel WQY Chinese font and the existing page-buffer display flow.

**Tech Stack:** ESP8266 Arduino C++, GxEPD2, U8g2 for Adafruit GFX, POSIX shell source verification, host C++17 assertions.

---

### Task 1: Specify Calendar Date and Geometry Behavior

**Files:**
- Modify: `test/test_dashboard_model.cpp`
- Test: `test/run_dashboard_tests.sh`

- [ ] **Step 1: Add failing assertions for Monday-first mapping and month lengths**

Add these assertions near the start of `main()`:

```cpp
  assert(mondayFirstColumn(1) == 0);
  assert(mondayFirstColumn(2) == 1);
  assert(mondayFirstColumn(0) == 6);

  assert(daysInGregorianMonth(2026, 1) == 31);
  assert(daysInGregorianMonth(2026, 2) == 28);
  assert(daysInGregorianMonth(2024, 2) == 29);
  assert(daysInGregorianMonth(2100, 2) == 28);
  assert(daysInGregorianMonth(2000, 2) == 29);
```

- [ ] **Step 2: Add failing assertions for cell placement and centered baselines**

Add:

```cpp
  CalendarCell mondayStart = calendarCellForDay(1, 1);
  assert(mondayStart.row == 0 && mondayStart.column == 0);
  CalendarCell sundayStart = calendarCellForDay(0, 1);
  assert(sundayStart.row == 0 && sundayStart.column == 6);
  CalendarCell nextMonday = calendarCellForDay(0, 2);
  assert(nextMonday.row == 1 && nextMonday.column == 0);

  TextPlacement oneDigit = centerTextInRect(5, 54, 41, 28, 10, 15, -3);
  assert(oneDigit.x == 20);
  assert(oneDigit.baseline_y == 74);
  TextPlacement twoDigits = centerTextInRect(5, 54, 41, 28, 20, 15, -3);
  assert(twoDigits.x == 15);
  assert(twoDigits.baseline_y == 74);
```

- [ ] **Step 3: Add failing assertions for Chinese labels and dynamic headers**

Add:

```cpp
  assert(strcmp(chineseWeekdayLabel(0), "周一") == 0);
  assert(strcmp(chineseWeekdayLabel(6), "周日") == 0);

  char calendarHeader[64] = {};
  formatChineseCalendarHeader(2026, 8, 30, 0,
                              calendarHeader, sizeof(calendarHeader));
  assert(strcmp(calendarHeader, "2026年8月30日 星期日") == 0);

  char weatherHeader[48] = {};
  formatChineseWeatherHeader(8, 30, weatherHeader, sizeof(weatherHeader));
  assert(strcmp(weatherHeader, "今天天气 8月30日") == 0);

  char weatherSummary[80] = {};
  formatChineseWeatherSummary(23.4f, 67, 3.2f,
                              weatherSummary, sizeof(weatherSummary));
  assert(strcmp(weatherSummary,
                "23.4°C 湿度67% 风速3.2km/h") == 0);
```

- [ ] **Step 4: Run the host test and verify RED**

Run:

```bash
sh test/run_dashboard_tests.sh
```

Expected: compilation fails because `mondayFirstColumn`, calendar structures,
formatters, and related helpers do not exist yet.

### Task 2: Implement Pure Calendar and Chinese Text Helpers

**Files:**
- Modify: `dashboard_model.h`
- Test: `test/test_dashboard_model.cpp`

- [ ] **Step 1: Add geometry data types and pure calendar helpers**

Add before the network model types:

```cpp
struct CalendarCell {
  int row;
  int column;
};

struct TextPlacement {
  int x;
  int baseline_y;
};

inline int mondayFirstColumn(int tm_wday) {
  return (tm_wday + 6) % 7;
}

inline int daysInGregorianMonth(int year, int month_one_based) {
  static const int lengths[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };
  if (month_one_based == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) ||
                      (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return lengths[month_one_based - 1];
}

inline CalendarCell calendarCellForDay(int first_tm_wday, int day) {
  const int index = mondayFirstColumn(first_tm_wday) + day - 1;
  return {index / 7, index % 7};
}

inline TextPlacement centerTextInRect(
    int x, int y, int width, int height, int text_width,
    int font_ascent, int font_descent) {
  const int text_height = font_ascent - font_descent;
  return {
    x + (width - text_width) / 2,
    y + (height - text_height) / 2 + font_ascent
  };
}
```

- [ ] **Step 2: Add Chinese label and formatter helpers**

Add:

```cpp
inline const char* chineseWeekdayLabel(int monday_first_column) {
  static const char* const labels[] = {
    "周一", "周二", "周三", "周四", "周五", "周六", "周日"
  };
  return labels[monday_first_column];
}

inline const char* chineseWeekdayName(int tm_wday) {
  static const char* const names[] = {
    "星期日", "星期一", "星期二", "星期三",
    "星期四", "星期五", "星期六"
  };
  return names[tm_wday];
}

inline char* formatChineseCalendarHeader(
    int year, int month, int day, int tm_wday,
    char* buffer, size_t buffer_size) {
  snprintf(buffer, buffer_size, "%d年%d月%d日 %s",
           year, month, day, chineseWeekdayName(tm_wday));
  return buffer;
}

inline char* formatChineseWeatherHeader(
    int month, int day, char* buffer, size_t buffer_size) {
  snprintf(buffer, buffer_size, "今天天气 %d月%d日", month, day);
  return buffer;
}

inline char* formatChineseWeatherSummary(
    float temperature, int humidity, float wind,
    char* buffer, size_t buffer_size) {
  snprintf(buffer, buffer_size,
           "%.1f°C 湿度%d%% 风速%.1fkm/h",
           temperature, humidity, wind);
  return buffer;
}
```

- [ ] **Step 3: Run the host test and verify GREEN**

Run:

```bash
sh test/run_dashboard_tests.sh
```

Expected: `Dashboard model tests PASS` with no compiler warnings.

- [ ] **Step 4: Check the focused diff**

Run:

```bash
git diff --check -- dashboard_model.h test/test_dashboard_model.cpp
```

Expected: no output.

- [ ] **Step 5: Commit the pure model slice when the dirty worktree permits**

```bash
git add dashboard_model.h test/test_dashboard_model.cpp
git commit -m "feat: add Chinese calendar layout helpers"
```

Do not commit this slice if doing so would include pre-existing user changes in
the same files; leave the tested changes unstaged and report that constraint.

### Task 3: Specify Renderer Wiring in the Source Gate

**Files:**
- Modify: `test/verify_pve_dashboard.sh`
- Test: `test/verify_pve_dashboard.sh`

- [ ] **Step 1: Add renderer requirements before production changes**

Add checks after the existing active-call checks:

```sh
for required_text in \
  'u8g2_font_wqy16_t_gb2312' \
  'u8g2_font_helvB14_tf' \
  'chineseWeekdayLabel' \
  'formatChineseCalendarHeader' \
  'formatChineseWeatherHeader' \
  'formatChineseWeatherSummary' \
  'centerTextInRect'; do
  if ! rg -qF "$required_text" "$code_source"; then
    echo "Chinese calendar/weather renderer is missing: $required_text" >&2
    exit 1
  fi
done

for obsolete_text in \
  'drawHeader(x, y, w, "Calendar")' \
  'drawHeader(x, y, w, "Weather")' \
  'const char* days[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"}' \
  'u8g2Fonts.setCursor(cx + 4, cy)'; do
  if rg -qF "$obsolete_text" "$active_source"; then
    echo "Obsolete calendar/weather renderer remains: $obsolete_text" >&2
    exit 1
  fi
done
```

- [ ] **Step 2: Run the source gate and verify RED**

Run:

```bash
sh test/verify_pve_dashboard.sh
```

Expected: fails with `Chinese calendar/weather renderer is missing` because the
sketch is not wired to the new helpers and fonts.

### Task 4: Render the Monday-First Chinese Calendar

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino:1510`
- Test: `test/verify_pve_dashboard.sh`

- [ ] **Step 1: Add a Chinese UTF-8 panel header helper**

Keep the existing `drawHeader()` for PVE/NAS and add:

```cpp
void drawChineseHeader(int x, int y, int w, const char* title) {
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2Fonts.drawUTF8(x + 5, y + 20, title);
  display.drawLine(x, y + 28, x + w, y + 28, GxEPD_BLACK);
}
```

- [ ] **Step 2: Replace `drawCalendar()` with measured cell layout**

Use these stable dimensions and tested helpers:

```cpp
void drawCalendar(int x, int y, int w, int h) {
  char header[64] = {};
  if (timeValid) {
    formatChineseCalendarHeader(
      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
      timeinfo.tm_mday, timeinfo.tm_wday, header, sizeof(header));
  } else {
    snprintf(header, sizeof(header), "时间不可用");
  }
  drawChineseHeader(x, y, w, header);
  if (!timeValid) return;

  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  struct tm first_day = timeinfo;
  first_day.tm_mday = 1;
  mktime(&first_day);

  const int start_x = x + 5;
  const int grid_y = y + 54;
  const int cell_w = (w - 10) / 7;
  const int cell_h = (h - 54) / 6;

  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  for (int column = 0; column < 7; ++column) {
    const char* label = chineseWeekdayLabel(column);
    const int label_width = u8g2Fonts.getUTF8Width(label);
    u8g2Fonts.drawUTF8(
      start_x + column * cell_w + (cell_w - label_width) / 2,
      y + 49, label);
  }

  u8g2Fonts.setFont(u8g2_font_helvB14_tf);
  for (int day = 1; day <= daysInGregorianMonth(year, month); ++day) {
    const CalendarCell cell = calendarCellForDay(first_day.tm_wday, day);
    const int cell_x = start_x + cell.column * cell_w;
    const int cell_y = grid_y + cell.row * cell_h;
    char day_text[3] = {};
    snprintf(day_text, sizeof(day_text), "%d", day);
    const TextPlacement text = centerTextInRect(
      cell_x, cell_y, cell_w, cell_h,
      u8g2Fonts.getUTF8Width(day_text),
      u8g2Fonts.getFontAscent(), u8g2Fonts.getFontDescent());

    if (day == timeinfo.tm_mday) {
      display.fillRect(cell_x + 2, cell_y + 2,
                       cell_w - 4, cell_h - 4, GxEPD_BLACK);
      u8g2Fonts.setForegroundColor(GxEPD_WHITE);
    }
    u8g2Fonts.drawUTF8(text.x, text.baseline_y, day_text);
    if (day == timeinfo.tm_mday) {
      u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    }
  }
}
```

- [ ] **Step 3: Run model and source tests**

Run:

```bash
sh test/run_dashboard_tests.sh
sh test/verify_pve_dashboard.sh
```

Expected: model tests pass; source gate still fails only on weather renderer
requirements that Task 5 has not implemented.

### Task 5: Render the Chinese Weather Header and Summary

**Files:**
- Modify: `epd5in83-hanshow-arduino.ino:1577`
- Test: `test/verify_pve_dashboard.sh`

- [ ] **Step 1: Replace the weather title and current summary**

At the start of `drawWeather()`, replace the English title and summary with:

```cpp
void drawWeather(int x, int y, int w, int h) {
  char header[48] = {};
  if (timeValid) {
    formatChineseWeatherHeader(
      timeinfo.tm_mon + 1, timeinfo.tm_mday, header, sizeof(header));
  } else {
    snprintf(header, sizeof(header), "今天天气");
  }
  drawChineseHeader(x, y, w, header);

  char buf[80] = {};
  formatChineseWeatherSummary(
    now_weather.temp, now_weather.humidity, now_weather.wind,
    buf, sizeof(buf));
  u8g2Fonts.setFont(u8g2_font_wqy16_t_gb2312);
  u8g2Fonts.drawUTF8(x + 5, y + 49, buf);
```

- [ ] **Step 2: Move the hourly graph below the larger summary**

Within the existing hourly block, change only these dimensions:

```cpp
    int cY = y + 55;
    int cH = 73;
```

Keep the existing separator at `y + 128` and the seven-day graph at
`y + 130`. The hourly graph then occupies rows 55 through 127 and cannot overlap
the summary baseline at row 49.

- [ ] **Step 3: Run the source gate and verify GREEN**

Run:

```bash
sh test/verify_pve_dashboard.sh
```

Expected: `PVE dashboard source checks PASS`.

- [ ] **Step 4: Run all host/source regression tests**

Run:

```bash
sh test/run_dashboard_tests.sh
sh test/verify_pve_dashboard.sh
sh test/run_partial_clock_tests.sh
sh test/verify_partial_clock.sh
```

Expected: all four scripts report PASS.

### Task 6: Document and Build the Finished Layout

**Files:**
- Modify: `README.md:10`
- Verify: `epd5in83-hanshow-arduino.ino`

- [ ] **Step 1: Update the screen layout description**

Change the two top-panel rows to:

```markdown
| 左上 | 中文年月日/星期标题、周一起始月历和居中当天高亮 |
| 右上 | 中文今天天气/日期、当前天气、8 小时和 7 天预报 |
```

Update the invalid-time sentence to say the calendar displays `时间不可用`.

- [ ] **Step 2: Run the production firmware build**

Run:

```bash
sh tools/build_firmware.sh
```

Expected: Arduino CLI completes successfully. Record Flash, IRAM, and RAM usage
and confirm the added Chinese font does not exceed device limits.

- [ ] **Step 3: Audit fixed geometry and requested strings**

Run:

```bash
rg -n 'wqy16|helvB14|周一|周日|formatChineseCalendarHeader|formatChineseWeatherHeader|centerTextInRect|cY = y \+ 55|cH = 73' epd5in83-hanshow-arduino.ino dashboard_model.h
```

Expected: each requested font, label, formatter, centering helper, and weather
chart offset is present in active source.

- [ ] **Step 4: Run final whitespace and worktree checks**

Run:

```bash
git diff --check
git status --short
```

Expected: no whitespace errors. Review status to ensure no generated build
artifacts or unrelated files were added.

- [ ] **Step 5: Commit only task-owned changes when safe**

```bash
git add README.md dashboard_model.h epd5in83-hanshow-arduino.ino \
  test/test_dashboard_model.cpp test/verify_pve_dashboard.sh
git commit -m "feat: localize calendar and weather display"
```

Skip this commit if any listed file contains pre-existing uncommitted user work
that cannot be isolated safely; preserve that work and report the final diff.
