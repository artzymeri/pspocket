import Foundation
import GameController

/// The game controller in the library (D-142): the d-pad or the left stick move the focus (once, again after 380 ms,
/// then every 130 ms while held), A (✕) plays, B (○) cancels, X (□) is Add Game, Menu is Settings. GameModel hands
/// the controller's state here while no game runs.
final class MenuInput {
    struct Actions {
        var move: (Int) -> Void
        var play: () -> Void
        var cancel: () -> Void
        var add: () -> Void
        var settings: () -> Void
        /// Any button or direction: the controller was the last input (the hints show).
        var used: () -> Void
    }

    var actions: Actions?
    private var a = false, b = false, x = false, menu = false
    private var direction = 0
    private var repeatTimer: Timer?

    func update(_ g: GCExtendedGamepad) {
        guard let actions else { return }
        var used = false
        func edge(_ pressed: Bool, _ was: inout Bool, _ action: () -> Void) {
            if pressed && !was {
                action()
                used = true
            }
            was = pressed
        }
        edge(g.buttonA.isPressed, &a, actions.play)
        edge(g.buttonB.isPressed, &b, actions.cancel)
        edge(g.buttonX.isPressed, &x, actions.add)
        edge(g.buttonMenu.isPressed, &menu, actions.settings)
        let stick = g.leftThumbstick.xAxis.value
        let dir = g.dpad.left.isPressed || stick < -0.5 ? -1 : g.dpad.right.isPressed || stick > 0.5 ? 1 : 0
        if dir != direction {
            direction = dir
            repeatTimer?.invalidate()
            if dir != 0 {
                actions.move(dir)
                used = true
                repeatTimer = Timer.scheduledTimer(withTimeInterval: 0.38, repeats: false) { [weak self] _ in
                    self?.repeatHeld()
                }
            }
        }
        if used { actions.used() }
    }

    private func repeatHeld() {
        guard direction != 0 else { return }
        actions?.move(direction)
        repeatTimer = Timer.scheduledTimer(withTimeInterval: 0.13, repeats: true) { [weak self] _ in
            guard let self, self.direction != 0 else { return }
            self.actions?.move(self.direction)
        }
    }
}
