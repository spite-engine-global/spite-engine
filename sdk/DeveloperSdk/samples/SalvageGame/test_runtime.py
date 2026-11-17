import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('runtime', Path(__file__).with_name('Runtime.py'))
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class Session:
    def __init__(self, modules):
        self.modules = modules
        self.calls = []
        self.reload_result = {'module': 'SalvageGame', 'warnings': []}

    def query(self, query, **kwargs):
        if query == 'modules':
            return {'modules': self.modules}
        if query == 'find':
            return {'total': 1, 'entities': [{'id': 7}]}
        if query == 'views':
            return {'gameView': 1}
        raise AssertionError(query)

    def command(self, command, **kwargs):
        self.calls.append(command)
        if command == 'loadModule':
            return {'module': 'SalvageGame', 'source': kwargs['path']}
        if command == 'reloadModule':
            return self.reload_result
        return {}


class StartupTest(unittest.TestCase):
    def test_loads_absent_module(self):
        session = Session([])
        runtime.start(session, Path('SalvageGame.dll'), True)
        self.assertEqual(session.calls[:3], ['loadModule', 'openScene', 'reloadModule'])

    def test_reloads_matching_preloaded_module(self):
        session = Session([{'name': 'SalvageGame', 'source': str(Path('SalvageGame.dll').resolve())}])
        runtime.start(session, Path('SalvageGame.dll'), True)
        self.assertEqual(session.calls[:2], ['openScene', 'reloadModule'])
        self.assertNotIn('loadModule', session.calls)

    def test_rejects_reload_warning_or_wrong_identity(self):
        for result in ({'module': 'SalvageGame', 'warnings': ['restore failed']},
                       {'module': 'Other', 'warnings': []}):
            session = Session([])
            session.reload_result = result
            with self.subTest(result=result), self.assertRaises(RuntimeError):
                runtime.start(session, Path('SalvageGame.dll'), True)
            self.assertNotIn('play', session.calls)

    def test_rejects_different_preloaded_build(self):
        session = Session([{'name': 'SalvageGame', 'source': 'other.dll'}])
        with self.assertRaisesRegex(RuntimeError, 'different source'):
            runtime.start(session, Path('SalvageGame.dll'), True)
        self.assertEqual(session.calls, [])


if __name__ == '__main__':
    unittest.main()


