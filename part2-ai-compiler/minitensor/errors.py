"""Error type shared by every minitensor stage.

Role: not a pipeline stage. Each stage raises MTError with the stage name and
a source position; driver.py turns it into a diagnostic for the CLI and UI.
"""


class MTError(Exception):
    def __init__(self, stage, message, line=0, col=0):
        super().__init__(message)
        self.stage = stage      # "parse", "graph", ...
        self.message = message
        self.line = line        # 1-based; 0 when there is no position
        self.col = col

    def to_dict(self):
        return {"stage": self.stage, "line": self.line, "col": self.col, "message": self.message}
