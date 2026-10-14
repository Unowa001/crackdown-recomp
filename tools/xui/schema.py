"""XUI v5 property wire schema. Names/types are format facts, not SDK code.

Each class declares only its own properties, in bit order. No defaults are
invented. Unused later SDK additions are deliberately omitted; unknown bits
stop semantic parsing. See docs/xui-format.md for sources and validation.
"""
CLASSES = {}


def define(name, base, fields=''):
    CLASSES['Xui' + name] = ('Xui' + base if base else None,
                             [tuple(field.split(':')) for field in fields.split()])


# This game's registered tables, dumped through 0x826D8F18 (Windows log 012).
# Later SDK fields must not be inserted: they change wire ordinals or cannot
# be consumed by this revision's loader.
define('Element', None, 'Id:s Width:f Height:f Position:v Scale:v Rotation:q Opacity:f Anchor:u Pivot:v Show:b BlendMode:u DisableTimelineRecursion:b DesignTime:b ColorWriteFlags:u')
for name in ('Canvas', 'Group', 'Visual'):
    define(name, 'Element')
define('Control', 'Element', 'ClassOverride:s Visual:s Enabled:b UnfocusedInput:b NavLeft:s NavRight:s NavUp:s NavDown:s Text:s PointSize:f ImagePath:s')
define('Scene', 'Control', 'DefaultFocus:s TransFrom:s TransTo:s TransBackFrom:s TransBackTo:s InterruptTransitions:u IgnorePresses:b')
define('Button', 'Control', 'PressKey:u')
define('NavButton', 'Button', 'PressPath:s StayVisible:b SrcTransIndex:u DestTransIndex:u')
define('BackButton', 'Button')
define('Checkbox', 'Control', 'PressKey:u')
define('RadioButton', 'Control', 'PressKey:u')
define('List', 'Control', 'Wrap:b')
define('CommonList', 'List', 'ItemsText:s ItemsImage:s ItemsNavPath:s')
define('ListItem', 'Checkbox', 'Layout:u Checkable:b SelectedSize:v KeepSizeUnfocused:b InterItemSpacing:v SmoothScroll:b SmoothScrollBaseSpeed:f SmoothScrollMaxSpeed:f SmoothScrollAcceleration:f')
define('Label', 'Control')
define('Slider', 'Control', 'RangeMin:i RangeMax:i Value:i Step:i Vertical:b AccelInc:i AccelTime:u')
define('ProgressBar', 'Control', 'RangeMin:i RangeMax:i Value:i')
define('Image', 'Element', 'SizeMode:u ImagePath:s BrushFlags:u TextureSurfaceElement:s LoadType:u')
define('ImagePresenter', 'Element', 'SizeMode:u DataAssociation:u BrushFlags:u LoadType:u')
define('Text', 'Element', 'Text:s TextColor:c DropShadowColor:c PointSize:f Font:s TextStyle:u LineSpacingAdjust:i TextScale:f')
define('TextPresenter', 'Element', 'TextColor:c DropShadowColor:c PointSize:f Font:s TextStyle:u LineSpacingAdjust:i DataAssociation:u TextScale:f')
define('Figure', 'Element', 'Stroke:o Fill:o Closed:b Points:r')
define('FigureStroke', None, 'StrokeWidth:f StrokeColor:c')
define('FigureFill', None, 'FillType:u FillColor:c TextureFileName:s Gradient:o Translation:v Scale:v Rotation:f WrapX:u WrapY:u BrushFlags:u TransformVersion:u')
define('FigureFillGradient', None, 'Radial:b NumStops:i StopColor:ac StopPos:af')
define('NineGrid', 'Element', 'TextureFileName:s LeftOffset:u TopOffset:u RightOffset:u BottomOffset:u NoCenter:b')
define('Video', 'Element', 'File:s SizeMode:u Pause:b Loop:b Volume:f')
define('Sound', 'Element', 'State:u Loop:b Finish:b Volume:f')
define('SoundXAudio', 'Sound', 'File:s')
define('RadioGroup', 'Control')
define('ScrollEnd', 'Control', 'Direction:u')
define('ScrollBar', 'Control', 'Direction:u MinThumbSize:u')
define('Caret', 'Control')
define('ComboBox', 'Control', 'ComboBoxStyle:u MaxVisibleVertItems:i')
define('Edit', 'Control', 'TextLimit:i AllowedChars:s PasswordChar:s ReadOnly:b Multiline:b SmoothScroll:b')
define('TabScene', 'Scene', 'TabCount:u Wrap:b UserInterrupt:b VerticalTabs:b NoAutoHide:b DefaultTab:u')
define('MessageBoxButton', 'Button', 'MessageBoxCenter:b MessageBoxVisual:s MessageBoxTitle:s MessageBoxText:s MessageBoxButtons:s')


def hierarchy(name):
    result = []
    while name:
        if name not in CLASSES:
            raise ValueError(f'unknown class schema {name!r}')
        result.append(name)
        name = CLASSES[name][0]
    return result
define('HtmlControl', 'Control', 'TeletypeCount:i')
define('HtmlElement', 'Element', 'Text:s TeletypeCount:i')
define('HtmlPresenter', 'Element', 'DataAssociation:i')
