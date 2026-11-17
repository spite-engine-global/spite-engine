"""Verify SalvageGame using independent world readback in a bounded session."""
import argparse
import json
from Runtime import project_session, start


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('project')
    args = parser.parse_args()
    actions = ('clock_right', 'clock_collect', 'clock_pause', 'clock_restart')
    with project_session(args.project, 600) as (session, module):
        playing = False
        try:
            start(session, module, paused=True)
            playing = True
            session.command('step', world='play', frames=2)
            def get(name):
                result = session.query('find', world='play', name=name,
                                       values=['TransformComponent', 'ClockworkState', 'Salvage'])
                if result['total'] != 1:
                    raise AssertionError(result)
                return result['entities'][0]
            def press(action, frames=1):
                session.command('injectAction', world='play', action=action, state='press', frames=frames)
                session.command('injectAction', world='play', action=action, state='release', frames=1)
            a = get('Salvage A')['TransformComponent']['position'][0]
            session.command('step', world='play', frames=30)
            b = get('Salvage A')['TransformComponent']['position'][0]
            assert b > a, (a, b)
            x = get('Collector')['TransformComponent']['position'][0]
            press('clock_right', 10)
            y = get('Collector')['TransformComponent']['position'][0]
            assert y > x, (x, y)
            press('clock_pause')
            assert get('Collector')['ClockworkState']['paused']
            p = get('Salvage A')['TransformComponent']['position'][0]
            session.command('step', world='play', frames=30)
            q = get('Salvage A')['TransformComponent']['position'][0]
            assert p == q, (p, q)
            press('clock_restart')
            player = get('Collector')
            state = player['ClockworkState']
            assert state['score'] == 0 and not state['paused'] and not state['won'], state
            assert abs(player['TransformComponent']['position'][0] + 4.0) < 0.001
            for expected in range(1, 4):
                press('clock_collect')
                player = get('Collector')
                state = player['ClockworkState']
                assert state['score'] == expected, state
            assert state['won'], state
            print('audio', json.dumps(session.query('audioStatus', world='play', id=player['id'])), flush=True)
            hud = session.query('uiTree', world='play')
            print('hud', json.dumps(hud), flush=True)
            nodes = {node['id']: node for node in hud['tree']}
            assert nodes['clock_score']['text'] == '3' and nodes['clock_won']['visible']
            fonts = session.query('uiFonts', world='play')
            print('fonts', json.dumps(fonts), flush=True)
            assert any(f['name'] == 'clockwork' and f['glyphs']['ready'] > 0 and
                       f['glyphs']['failed'] == 0 and f['glyphs']['pending'] == 0 for f in fonts['fonts'])
            screenshot = session.command('screenshot', panel='Game', path='salvage-check.png')
            print('screenshot', json.dumps(screenshot), flush=True)
            press('clock_restart')
            state = get('Collector')['ClockworkState']
            assert state['score'] == 0 and not state['won'] and not state['paused'], state
            print('PASS', json.dumps({'cargo': [a, b], 'player': [x, y],
                                      'pausedCargo': [p, q], 'restart': state}), flush=True)
        finally:
            if playing:
                for action in actions:
                    try:
                        session.command('injectAction', world='play', action=action, state='release', frames=0)
                    except Exception as error:
                        print('release failed', action, error, flush=True)
                session.command('stop')


if __name__ == '__main__':
    main()

