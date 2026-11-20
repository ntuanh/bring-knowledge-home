// ai_part.js - Part 2 (mini AI compiler) stage definitions for the visualizer.
'use strict';

App.parts.ai = {
  id: 'ai',
  defaultExample: '01_mlp_layer.mt',
  stages: [{ id: 'todo', label: 'Coming soon', help: 'This part is built in a later milestone.', status: () => 'idle', render: () => {} }],
  compile: async () => { throw new Error('Part 2 is not implemented yet.'); },
};
