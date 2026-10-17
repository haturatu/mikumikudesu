"""Validate complete UTF-8 catalogs, printf safety and literal UI coverage without a GPU."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
STRING = r'"(?:[^"\\]|\\.)*"'
PRINTF = re.compile(r'%(?:[-+ #0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn%])')


def literal(expression):
    return ''.join(json.loads(token) for token in re.findall(STRING, expression))


class CatalogTests(unittest.TestCase):
    def test_catalog(self):
        entries = {}
        for line in (ROOT / 'src/ui/i18n_catalog.inc').read_text(encoding='utf-8').splitlines():
            values = [json.loads(token) for token in re.findall(STRING, line)]
            self.assertEqual(len(values), 5, line)
            key, *translations = values
            self.assertNotIn(key, entries)
            entries[key] = translations
            for value in translations:
                self.assertTrue(value.strip(), key)
                self.assertNotIn('\ufffd', value)
                self.assertEqual(PRINTF.findall(key), PRINTF.findall(value), key)
                self.assertNotIn('##', value, key)
                self.assertEqual(sorted(re.findall(r'\{\d\}', key)), sorted(re.findall(r'\{\d\}', value)), key)
        for source in ('src/app/application.cpp', 'src/ui/keyframe_editor.cpp', 'src/ui/i18n_imgui.hpp'):
            text = (ROOT / source).read_text(encoding='utf-8')
            unsafe = r'ImGui::(?:Text|TextWrapped|TextDisabled|SetTooltip)\(\s*ui::tr\(\s*' + STRING + r'(?:\s*' + STRING + r')*\s*\)\s*\)'
            self.assertIsNone(re.search(unsafe, text), source)
            widgets = 'Button|SmallButton|BeginMenu|MenuItem|Checkbox|CheckboxFlags|RadioButton|CollapsingHeader|TreeNode|Selectable|InputText|InputTextWithHint|InputInt|InputScalar|SliderFloat|SliderInt|DragFloat|DragInt|ColorEdit3|BeginCombo|Combo|SeparatorText|TextUnformatted|TableSetupColumn'
            for direct in re.finditer(r'ImGui::(?:' + widgets + r')\(\s*(' + STRING + r')', text):
                value = json.loads(direct[1])
                self.assertFalse(not value.startswith('##') and re.search('[A-Za-z]', value), (source, value))
            for match in re.finditer(r'(?:(?:ui::)?(tr|label|items|Message)|(?:workspaceButton|workspaceWindowName|hint))\(\s*(' + STRING + r'(?:\s*' + STRING + r')*)\s*(?:\)|,)', text):
                value = literal(match[2])
                keys = value.split('|') if match[1] == 'items' else [value.split('##')[0]]
                for key in keys:
                    self.assertIn(key, entries, (source, key))
        self.assertGreater(len(entries), 350)


if __name__ == '__main__':
    unittest.main()
