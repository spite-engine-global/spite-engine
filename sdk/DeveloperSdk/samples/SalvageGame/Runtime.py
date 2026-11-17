"""Shared project selection and bounded runtime setup for SalvageGame."""
from contextlib import contextmanager
import json
import signal
import threading
import os
from pathlib import Path
import subprocess
import sys


@contextmanager
def project_session(project, lifetime, sdk=None):
    if type(lifetime) is not int or lifetime <= 0:
        raise ValueError("lifetime must be a positive number of seconds")
    project = Path(project).resolve(strict=True)
    sdk = Path(sdk or Path(__file__).resolve().parents[2]).resolve(strict=True)
    launcher = sdk / ('sdk.cmd' if os.name == 'nt' else 'sdk.sh')
    prefix = [str(launcher)] if os.name == 'nt' else ['sh', str(launcher)]
    result = subprocess.run([*prefix, 'project', str(project)], check=True, capture_output=True, text=True)
    info = json.loads(result.stdout)
    module = project / 'build' / ('SalvageGame.dll' if os.name == 'nt' else 'libSalvageGame.so')
    if not module.is_file():
        raise FileNotFoundError(f'{module}; build the project first')
    sys.path.insert(0, str(sdk / 'tools/SpiteCli'))
    from spite_agent import Session
    if info['Cli']:
        with Session.run(executable=info['Cli'], lifetime=lifetime,
                         engine_args=info['HostArgs'], engine_log=str(project / 'salvage-engine.log')) as session:
            yield session, module
    else:
        # A separate process group lets the owner stop both the SDK wrapper and host.
        argv = [sys.executable, '-c', 'import os,sys; os.setsid(); os.execvp(sys.argv[1], sys.argv[1:])',
                *prefix, 'run', str(project)]
        session = Session(argv, engine_log=str(project / 'salvage-engine.log'))
        def terminate():
            try:
                os.killpg(session._process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        timer = threading.Timer(lifetime, terminate)
        try:
            timer.start()
        except BaseException:
            terminate()
            session.close()
            raise
        try:
            with session:
                yield session, module
        finally:
            timer.cancel()
            timer.join()
            terminate()



def start(session, module, paused):
    loaded = [entry for entry in session.query('modules')['modules'] if entry['name'] == 'SalvageGame']
    if loaded:
        if len(loaded) != 1 or Path(loaded[0]['source']).resolve() != Path(module).resolve():
            raise RuntimeError(f'SalvageGame is loaded from a different source: {loaded}')
    else:
        result = session.command('loadModule', path=str(module))
        if result['module'] != 'SalvageGame' or Path(result['source']).resolve() != Path(module).resolve():
            raise RuntimeError(f'Unexpected loaded module: {result}')
        print('load', result, flush=True)
    print('open', session.command('openScene', world='scene', path='SalvageGame.json', force=True), flush=True)
    reload = session.command('reloadModule', name='SalvageGame')
    print('reload', reload, flush=True)
    if reload['module'] != 'SalvageGame' or reload['warnings']:
        raise RuntimeError(reload)
    camera = session.query('find', world='scene', name='Game camera')
    if camera['total'] != 1:
        raise RuntimeError(f'Expected one authored camera: {camera}')
    view = session.query('views')['gameView']
    print('camera', session.command('bindCamera', world='scene', view=view,
                                    id=camera['entities'][0]['id']), flush=True)
    print('play', session.command('play', paused=paused), flush=True)
    session.command('gameFocus', on=True)

