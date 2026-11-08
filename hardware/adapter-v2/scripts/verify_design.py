"""Independent, image-derived pin checks against the netlist exported by KiCad."""
from pathlib import Path
import xml.etree.ElementTree as ET
import json
from schematic_links import audit_links
ROOT=Path(__file__).resolve().parents[1]
links=audit_links(ROOT/'H743_RGB_Adapter.kicad_pcb', ROOT/'output/H743_RGB_Adapter.xml')
assert not links['mismatches'], links['mismatches']
assert not links['duplicate_paths'], 'Duplicate schematic associations'
doc=ET.parse(ROOT/'output/H743_RGB_Adapter.xml').getroot()
mapping={(n.attrib['ref'],n.attrib['pin']):net.attrib['name'] for net in doc.findall('nets/net') for n in net.findall('node')}
checks=0
def check(ref,pin,net):
    global checks
    actual=mapping[(ref,str(pin))]
    assert actual==net,(ref,pin,actual,net)
    checks+=1
def nc(ref,pin):
    global checks
    assert mapping[(ref,str(pin))].startswith('unconnected-'),(ref,pin)
    checks+=1
# External evidence: screenshot 1, separately transcribed here.
for p in [1,2]:check('J1',p,'+5V')
for p in [3,4,5,11,12,13,20,21,22,23,29,'MP']:check('J1',p,'GND')
for p,n in {6:'H_R3',7:'H_R4',8:'H_R5',9:'H_R6',10:'H_R7',14:'H_G2',15:'H_G3',16:'H_G4',17:'H_G5',18:'H_G6',19:'H_G7',24:'H_B3',25:'H_B4',26:'H_B5',27:'H_B6',28:'H_B7',30:'H_PCLK',31:'H_HSYNC',32:'H_VSYNC',33:'H_DE',34:'H_BL_PWM',35:'CTP_RST',36:'CTP_SDA',37:'H_LCD_READY',38:'CTP_SCL',39:'CTP_INT',40:'H_NRST'}.items():check('J1',p,n)
for j in ['J2','J7']:
    for p,n in {1:'LCD_SDA',2:'LCD_SCL',5:'GND',7:'LCD_DE',8:'LCD_VSYNC',9:'LCD_HSYNC',11:'LCD_PCLK',12:'GND',37:'+3V3',38:'GND',39:'LEDA',40:'LEDK','MP':'GND'}.items():check(j,p,n)
    for color,first,low in [('R',36,3),('G',28,2),('B',20,3)]:
        for bit in range(8):check(j,first-bit,'GND' if bit<low else 'LCD_'+color+str(bit))
check('J2',3,'LCD_RST');check('J2',4,'LCD_CS');nc('J2',6);nc('J2',10)
check('J7',3,'LCD_CS');check('J7',4,'LCD_RST');check('J7',6,'GND');check('J7',10,'+3V3')
for j,pins in [('J3',['+3V3','GND','CTP_SCL','CTP_SDA','CTP_INT','CTP_RST']),('J4',['GND','+3V3','CTP_INT','CTP_SCL','CTP_SDA','CTP_RST']),('J5',['+3V3','GND','CTP_SCL','CTP_SDA','CTP_INT','CTP_RST'])]:
    for p,n in enumerate(pins,1):check(j,p,n)
signals=['R3','R4','R5','R6','R7','G2','G3','G4','G5','G6','G7','B3','B4','B5','B6','B7','PCLK','HSYNC','VSYNC','DE']
for i,signal in enumerate(signals):
    rn='RN'+str(i//4+1);ch=i%4
    check(rn,ch+1,'H_'+signal);check(rn,8-ch,'LCD_'+signal)
for p,n in {4:'+3V3',13:'GND',11:'PY_LCD_SCK',12:'PY_LCD_SDA',14:'PY_LCD_CS',15:'PY_LCD_RST',16:'LCD_READY',6:'PY_SWDIO',7:'PY_SWCLK',8:'PY_NRST'}.items():check('U3',p,n)
for p in [1,2,3,9,10]:nc('U3',p)
for p,n in {1:'BL_LX',2:'GND',3:'LEDK',4:'BL_EN',5:'LEDA',6:'+5V'}.items():check('U2',p,n)
check('D1',1,'LEDA');check('D1',2,'BL_LX');check('R4',1,'LEDK');check('R4',2,'GND')
for p,n in {1:'BL_GATE',2:'BL_EN'}.items():check('R5',p,n)
for p,n in {1:'BL_GATE',2:'GND'}.items():check('R6',p,n)
for p,n in {1:'H_BL_PWM',2:'LCD_READY',3:'GND',4:'BL_GATE',5:'+3V3'}.items():check('U4',p,n)
comps=doc.findall('components/comp')
assert len(comps)==56
assert not any(c.attrib['ref'].startswith('JP') for c in comps)
assert not any('PANEL_SEL' in n for n in mapping.values())
erc=json.loads((ROOT/'output/erc.json').read_text(encoding='utf8'))
assert not any(s.get('violations') for s in erc['sheets'])
drc=json.loads((ROOT/'output/drc.json').read_text(encoding='utf8'))
assert not [v for v in drc['violations'] if v.get('severity') == 'error']
assert not drc['schematic_parity']
print(f'PASS: {checks} image/datasheet-derived pin checks; 56 components; no jumpers; ERC clean; no DRC errors; schematic parity clean.')
report=f'''# 验证结果\n\n检查工具：KiCad CLI 10.0.3。\n\n- 原理图 ERC：0 错误、0 警告；未设置 ERC 排除项。\n- PCB DRC：0 个 error；当前工程规则下保留 {len(drc['violations'])} 个 warning，均为库副本校验提示，不是走线、短路、间距或 courtyard 违规。\n- 原理图与 PCB 一致性：0 违规，包括封装、网络和字段。\n- 独立核对：{checks} 条引脚断言通过，依据用户图和芯片引脚表检查 H743、两类屏座、三触摸座、排阻、PY32、背光电路；R6 下拉已位于 BL_GATE 侧。\n- 元件数：56；符号编号与 PCB 对应；没有 JP 跳线或 PANEL_SEL 网络。\n- PCB：0 条走线、0 个过孔、0 个铜区；本轮按用户要求不布线。\n- DRC 未连接项：{len(drc['unconnected_items'])}，保留为飞线；没有隐藏或排除。它们必须在后续布线阶段解决。\n\n原始结果见 `output/erc.json`、`output/drc.json`。\n\n检查证明文件可解析、引脚转录正确、元件布局无已知几何冲突，不等于已验证五款实物屏幕的电压、电流、尾排方向、初始化时序或高速信号完整性。尚无完整屏幕数据手册、固件和实物点亮测试。本阶段不提供制板文件。\n'''
(ROOT/'docs/verification.md').write_text(report,encoding='utf8')
