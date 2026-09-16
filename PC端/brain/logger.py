# logger.py — 统一日志(INFO 默认,DEBUG 可选)
"""2026-09-16:初版"""
import logging
import time


def get_logger(name: str, level: int = logging.INFO) -> logging.Logger:
    """简单统一 logger(单进程,不打 stdout 缓冲问题)"""
    logger_ = logging.getLogger(name)
    if not logger_.handlers:
        h = logging.StreamHandler()
        h.setFormatter(
            logging.Formatter(
                "%(asctime)s.%(msecs)03d [%(name)s] %(levelname)s: %(message)s",
                datefmt="%H:%M:%S",
            )
        )
        logger_.addHandler(h)
        logger_.setLevel(level)
        logger_.propagate = False
    return logger_