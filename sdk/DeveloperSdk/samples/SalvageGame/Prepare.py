"""Create, build and smoke-test a fresh SalvageGame project with an installed SDK."""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import wave
from Runtime import project_session


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True)
    args = parser.parse_args()
    sdk, project = args.sdk.resolve(strict=True), args.project.resolve()
    if project.exists() or project.is_relative_to(sdk):
        raise ValueError('Choose a new external project directory')
    launcher = sdk / ('sdk.cmd' if os.name == 'nt' else 'sdk.sh')
    prefix = [str(launcher)] if os.name == 'nt' else ['sh', str(launcher)]
    def sdk_call(*arguments, capture=False):
        return subprocess.run([*prefix, *map(str, arguments)], check=True, text=True,
                              capture_output=capture).stdout
    sdk_call('new', project, '--name', 'SalvageGame', '--content-only')
    for name in ('CMakeLists.txt', 'SalvageGame.cpp'):
        shutil.copyfile(Path(__file__).parent / name, project / name)
    (project / 'ship.json').write_text(json.dumps({'format': 1, 'modules': ['SalvageGame'],
        'files': ['config/project/input/actions.json', 'scenes/SalvageGame.json'],
        'scene': 'scenes/SalvageGame.json'}, indent=2), encoding='utf-8')
    with wave.open(str(project / 'assets/audio/salvage.wav'), 'wb') as sound:
        sound.setparams((1, 2, 22050, 0, 'NONE', 'not compressed'))
        sound.writeframes(b''.join(struct.pack('<h', int(12000 * (1-i/4410) *
            math.sin(2 * math.pi * (440+400*i/4410) * i/22050))) for i in range(4410)))
    sdk_call('build', project)
    with project_session(project, 600, sdk) as (session, module):
        def command(operation, **kw):
            result = session.command(operation, **kw)
            print(json.dumps({'command': operation, 'args': kw, 'result': result}), flush=True)
            return result
        def query(operation, **kw):
            result = session.query(operation, **kw)
            print(json.dumps({'query': operation, 'args': kw, 'result': result}), flush=True)
            return result
        command('loadModule', path=str(module))
        for name, kind, position, scale in [
            ('Floor', 'cube', [0,-.25,0], [18,.5,14]),
            ('Belt A', 'cube', [0,.2,0], [12,.3,2]),
            ('Belt B', 'cube', [0,.2,4], [12,.3,2]),
            ('Salvage A', 'torus', [-4,.8,0], [.7,.7,.7]),
            ('Salvage B', 'cube', [-4,.8,0], [.7,.7,.7]),
            ('Salvage C', 'cylinder', [-4,.8,4], [.7,.7,.7]),
            ('Collector', 'capsule', [-4,1,1], [.6,.6,.6])]:
            entity = command('createPrimitive', world='scene', name=name, kind=kind,
                             transform={'position': position, 'scale': scale})['id']
            if name.startswith('Salvage '):
                command('addComponent', world='scene', id=entity, component='Salvage',
                        value={'startX': -4.0, 'speed': 1.0})
            if name == 'Collector':
                command('addComponent', world='scene', id=entity, component='ClockworkState')
                command('addComponent', world='scene', id=entity, component='AudioEmitterComponent',
                        value={'clip':'salvage.wav', 'playing':False, 'loop':False, 'spatial':False, 'volume':0.5, 'stream':False})
                command('addComponent', world='scene', id=entity, component='PointLightComponent',
                        value={'color':[0.2,0.7,1.0], 'intensity':0, 'radius':4, 'castShadows':False})
        for x in (-4, 0, 4):
            light = command('createEntity', world='scene', name=f'Light {x}')['id']
            command('set', world='scene', id=light, path='TransformComponent.position', value=[x,4,0])
            command('addComponent', world='scene', id=light, component='PointLightComponent',
                    value={'color':[1.0,0.75,0.45], 'intensity':100.0, 'radius':12.0, 'castShadows':False})
        camera = command('createEntity', world='scene', name='Game camera')['id']
        command('set', world='scene', id=camera, path='TransformComponent.position', value=[0,14,-14])
        command('set', world='scene', id=camera, path='TransformComponent.rotation', value=[0,0.9238795,0.3826834,0])
        command('addComponent', world='scene', id=camera, component='CameraComponent')
        command('saveScene', world='scene', path='SalvageGame.json')
        reload = command('reloadModule', name='SalvageGame')
        if reload['warnings']:
            raise RuntimeError(reload)
        command('openScene', world='scene', path='SalvageGame.json', force=True)
        scene = query('find', world='scene', name='Collector', values=['ClockworkState'])
        assert scene['total'] == 1 and scene['entities'][0]['ClockworkState']['score'] == 0
        command('play', paused=True)
        command('step', world='play', frames=2)
        player = query('find', world='play', name='Collector', values=['ClockworkState'])
        assert player['total'] == 1 and player['entities'][0]['ClockworkState']['score'] == 0
        fonts = query('uiFonts', world='play')
        assert any(font['name'] == 'clockwork' and font['glyphs']['failed'] == 0
                   and font['glyphs']['ready'] > 0 for font in fonts['fonts'])
        command('stop')
    print('Prepared and smoke-tested', project)


if __name__ == '__main__':
    main()



