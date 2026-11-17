"""Launch the prepared game in a bounded editor session."""
import argparse
import threading
from Runtime import project_session, start


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('project')
    parser.add_argument('--timeout', type=int, default=1800, help='host lifetime in seconds (positive)')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    with project_session(args.project, args.timeout) as (session, module):
        playing = False
        try:
            start(session, module, paused=False)
            playing = True
            finished = threading.Event()
            def wait_for_enter():
                try:
                    input('Game focused. WASD move, Space collect, P pause, R restart. Press Enter to close.\n')
                except EOFError:
                    pass
                finally:
                    finished.set()
            threading.Thread(target=wait_for_enter, daemon=True).start()
            if not finished.wait(args.timeout):
                print('Host lifetime expired', flush=True)
        finally:
            if playing:
                try:
                    session.command('stop')
                except (ConnectionError, TimeoutError) as error:
                    print('Host already closed:', error, flush=True)
    print('Session closed', flush=True)


if __name__ == '__main__':
    main()

