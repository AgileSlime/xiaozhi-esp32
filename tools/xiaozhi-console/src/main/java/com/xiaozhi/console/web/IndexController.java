package com.xiaozhi.console.web;

import org.springframework.stereotype.Controller;
import org.springframework.web.bind.annotation.GetMapping;

/**
 * 根路径跳到静态首页 index.html（在 src/main/resources/static/ 下）。
 *
 * <p>Spring Boot 默认就会把 / 当作 index.html，但显式映射能避免
 * 某些情况下因为加了 @RestController 而被覆盖。
 */
@Controller
public class IndexController {

    @GetMapping("/")
    public String index() {
        return "forward:/index.html";
    }
}
