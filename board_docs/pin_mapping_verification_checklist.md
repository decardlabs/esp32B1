# Pin Mapping Verification Checklist

Date: 2026-05-29
Board: LYIT_ESP32S3MB
Source files: pin_mapping.json, pin_mapping_firmware.json, ESP32S3电路图.pdf

## Usage

1. Verify one item at a time directly from the schematic.
2. Record final conclusion and evidence (sheet, component, net label).
3. Update pin_mapping.json and pin_mapping_firmware.json immediately after each completed item.

## Item 1: GPIO45/46/47/48 exact net roles

Status: [x] DONE

Steps:
1. Open page1 (main sheet) and find U1 pins IO45/IO46/IO47/IO48.
2. Trace each net label from U1 pin to destination.
3. Confirm whether each pin goes to camera, header, translator, or reserved net.
4. Add final mapping in both JSON files under confirmed mapping sections.

Evidence template:
- Sheet: page1
- U1 pin refs: IO45, IO46, IO47, IO48
- Net labels found: OV_PCLK, I2S_SCK, OV_VSYNC, OV_HREF
- Final mapping:
   - IO45 -> OV_PCLK
   - IO46 -> I2S_SCK
   - IO47 -> OV_VSYNC
   - IO48 -> OV_HREF

## Item 2: OV_SDA and OV_SCL exact ESP32 pins

Status: [x] DONE

Steps:
1. Open page3 (OV2640 sheet), locate OV_SDA and OV_SCL nets at J15.
2. Cross-probe to page1 where OV_SDA/OV_SCL connect to U1 or intermediary logic.
3. Confirm final ESP32 GPIO numbers and whether level translation exists in path.
4. Write final mapping and remove this item from unresolved list.

Evidence template:
- Sheet: page3 -> page1
- Net labels found: OV_SDA, OV_SCL
- Translation/buffer path: 未见额外电平转换器，按同名网直连
- Final mapping:
   - OV_SDA -> IO39
   - OV_SCL -> IO38

## Item 3: XL9555 IO1_6 and IO1_7 net labels

Status: [x] DONE

Steps:
1. Locate XL9555 symbol on page1.
2. Trace IO1_6 and IO1_7 pins to their net labels.
3. Confirm final function names and destination components.
4. Add IO1_6/IO1_7 to io_expander.ports.P1 in both JSON files.

Evidence template:
- Sheet: page1
- XL9555 pin refs: IO1_6, IO1_7
- Net labels found: LED3, LED4
- Final mapping:
   - IO1_6 -> LED3
   - IO1_7 -> LED4

## Item 4: LED subsystem architecture (discrete vs WS2812 chain)

Status: [x] DONE

Current confirmed notes:
- IO1_0 -> 空
- IO1_1 -> TFT_BLK
- IO1_2 -> 空
- IO1_3 -> TFT_RES
- IO1_4 -> LED1
- IO1_5 -> LED2
- IO1_6 -> LED3
- IO1_7 -> LED4

Steps:
1. Locate LED1-LED4 and any WS2812 symbols on page1.
2. Confirm whether LED1-LED4 are direct GPIO/expander outputs or WS2812 daisy-chain nodes.
3. Confirm role of TXS0108E in LED data path, if present.
4. Normalize naming in JSON:
   - Option A: LED_IND1..LED_IND4 for discrete indicators
   - Option B: RGB_CHAIN for WS2812 serial bus
   - Option C: both, with separate sections

Evidence template:
- Sheet: page1
- Components found: LED1, LED2, LED3, LED4
- Data/control path: XL9555 IO1_4/IO1_5/IO1_6/IO1_7 分别控制 LED1/LED2/LED3/LED4
- Final architecture: 离散 LED 指示灯架构 (非 WS2812 串行灯带)

## Item 5: Camera rail naming final cross-check

Status: [x] DONE

Steps:
1. On page3, list all camera rails at J15 (for example A VDD, DOVDD, 1.3V).
2. Trace each rail to its regulator source (U8/U9 and related passives).
3. Map each rail to camera domain naming (analog, digital IO, core) using schematic net names.
4. Update camera power section in pin_mapping.json with unambiguous wording.

Evidence template:
- Sheet: page3
- Rail list from J15: A VDD, DOVDD, 1.3V
- Regulator source: A VDD/DOVDD 由 LDO_2.8V 提供；1.3V 由 LDO_1.3V 提供
- Final domain map:
   - A VDD -> LDO 2.8V 的 AVDD
   - DOVDD -> LDO 2.8V 的 DOVDD
   - 1.3V -> LDO 1.3V
   - DVDD -> 未见独立网名

## Completion Criteria

1. unresolved array in pin_mapping_firmware.json is empty.
2. pin_mapping.json no longer contains uncertain words like "可能" or "待确认" in confirmed sections.
3. Both JSON files pass parse check.

## Final Validation Commands

Run after all items are done:

- /Users/macairm5/Documents/esp32/test002/.venv/bin/python -c "import json; json.load(open('pin_mapping.json',encoding='utf-8')); print('pin_mapping.json OK')"
- /Users/macairm5/Documents/esp32/test002/.venv/bin/python -c "import json; json.load(open('pin_mapping_firmware.json',encoding='utf-8')); print('pin_mapping_firmware.json OK')"
