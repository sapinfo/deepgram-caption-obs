# OBS Plugin Button Text Toggle Fix Guide

## Problem

`obs_properties_add_button()`으로 만든 버튼의 텍스트가 클릭 후 변경되지 않음.

### 원인

- 버튼 콜백에서 `return true`는 `RefreshProperties()`만 호출
- `RefreshProperties()`는 기존 properties 객체에서 UI를 다시 그림
- `get_properties()`를 다시 호출하지 않음 (그건 `ReloadProperties()`)
- 따라서 `get_properties()`에서 조건부로 버튼 텍스트를 설정하는 방식은 동작하지 않음

### 해결

콜백 내에서 `obs_property_set_description()`으로 직접 버튼 텍스트를 변경.
`RefreshProperties()`가 변경된 description을 읽어서 UI에 반영.

---

## Fix

### Before (동작 안 함)

```cpp
static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *, void *private_data)
{
    auto *data = static_cast<my_plugin_data *>(private_data);

    // ... 설정 읽기 ...

    if (data->captioning) {
        stop_captioning(data);
    } else {
        start_captioning(data);
    }
    return true;
}
```

### After (동작함)

```cpp
static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *property, void *private_data)
{
    auto *data = static_cast<my_plugin_data *>(private_data);

    // ... 설정 읽기 ...

    if (data->captioning) {
        stop_captioning(data);
        obs_property_set_description(property, "Start Caption");
    } else {
        start_captioning(data);
        obs_property_set_description(property, "Stop Caption");
    }
    return true;
}
```

---

## Key Points

1. 두 번째 파라미터를 `obs_property_t *` -> `obs_property_t *property`로 이름 부여
2. 분기 안에서 `obs_property_set_description(property, "새 텍스트")` 호출
3. `return true`는 그대로 유지 (RefreshProperties가 변경된 description을 읽음)
4. `get_properties()`의 초기 텍스트 설정도 상태 기반으로 유지 (Properties 창 처음 열 때 사용)

---

## Apply to Each Project

### Soniox Caption Plugin

파일: `src/plugin-main.cpp`

```cpp
// Find:
static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *, void *private_data)

// Replace with:
static bool on_start_stop_clicked(obs_properties_t *, obs_property_t *property, void *private_data)

// Add inside the if/else block:
    if (data->captioning) {
        stop_captioning(data);
        obs_property_set_description(property, "Start Caption");
    } else {
        start_captioning(data);
        obs_property_set_description(property, "Stop Caption");
    }
```

### Speechmatics Caption Plugin

파일: `src/plugin-main.cpp` (동일 패턴)

```cpp
// Same fix: add obs_property_t *property parameter name
// Same fix: add obs_property_set_description() calls in if/else
```

### ElevenLabs Caption Plugin

파일: `src/plugin-main.cpp` (동일 패턴)

```cpp
// Same fix: add obs_property_t *property parameter name
// Same fix: add obs_property_set_description() calls in if/else
```

---

## OBS Internal Reference

Source: `obs-studio/shared/properties-view/properties-view.cpp`

```
Button clicked -> callback returns true
  -> QMetaObject::invokeMethod(view, "RefreshProperties")  // NOT ReloadProperties
  -> Rebuilds UI from existing obs_properties_t object
  -> obs_property_description(prop) reads updated description
```

- `RefreshProperties()`: Rebuilds widgets from existing properties (lightweight)
- `ReloadProperties()`: Calls get_properties() for fresh properties (heavyweight)
- Button click only triggers RefreshProperties
