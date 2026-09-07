'use strict';

// The repository has browser scripts and CommonJS test tooling. Keep the
// initial rules small and run them only on files selected by the incremental
// checker so legacy code is not turned into a noisy all-repository baseline.
module.exports = [
  {
    ignores: [
      'build/**',
      'dist/**',
      'third_party/**',
      'vendor/**',
    ],
  },
  {
    files: ['cpp/web/**/*.js'],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'script',
    },
    rules: {
      eqeqeq: 'error',
      'no-constant-binary-expression': 'error',
      'no-unused-vars': ['warn', {argsIgnorePattern: '^_'}],
    },
  },
  {
    files: ['**/*.cjs', 'scripts/**/*.js'],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'commonjs',
    },
    rules: {
      eqeqeq: 'error',
      'no-constant-binary-expression': 'error',
      'no-unused-vars': ['warn', {argsIgnorePattern: '^_'}],
    },
  },
];
