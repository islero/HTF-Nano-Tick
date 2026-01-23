# Contributing to HFT NanoTick

Thank you for your interest in contributing to HFT NanoTick! This document provides guidelines for contributing to the project.

## Getting Started

1. Fork the repository
2. Clone your fork: `git clone https://github.com/your-username/HTF-Nano-Tick.git`
3. Create a feature branch: `git checkout -b feature/your-feature-name`
4. Make your changes
5. Run tests and ensure they pass
6. Submit a pull request

## Development Setup

```bash
# Clone and build
git clone https://github.com/your-username/HTF-Nano-Tick.git
cd HTF-Nano-Tick
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DHFT_ENABLE_SANITIZERS=ON ..
cmake --build . -j$(nproc)

# Run tests
ctest --output-on-failure
```

## Code Style

- Follow modern C++20 conventions
- Use `clang-format` for code formatting (configuration in `.clang-format`)
- Keep functions small and focused
- Prefer `constexpr` and compile-time computation where possible
- Document public APIs with clear comments

## Performance Guidelines

Since this is an HFT-focused project, performance is critical:

- **No allocations in hot paths**: Use pre-allocated pools and arenas
- **Cache efficiency**: Align data structures to cache lines (`alignas(64)`)
- **Lock-free first**: Prefer atomic operations over mutexes
- **Measure, don't guess**: Include benchmarks for performance-critical changes

## Pull Request Process

1. Ensure all tests pass locally
2. Update documentation if adding new features
3. Add tests for new functionality
4. Keep commits focused and atomic
5. Write clear commit messages

## Reporting Issues

When reporting issues, please include:

- Description of the problem
- Steps to reproduce
- Expected vs actual behavior
- System information (OS, compiler, version)

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
